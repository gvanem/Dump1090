/**
 * \file    aircraft.c
 * \ingroup Main
 * \brief   Handling of aircraft data and ICAO address utilities
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "misc.h"
#include "geo.h"
#include "interactive.h"
#include "smartlist.h"
#include "sqlite3.h"
#include "zip.h"
#include "cpr.h"
#include "net_io.h"
#include "aircraft.h"

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
 * \def JSON_BUF_LEN
 * The initial and increment buffer-size in `aircraft_make_json()`
 */
#define JSON_BUF_LEN  (20*1024)

/**
 * \def OUTLINE_PERIOD
 * The period for writing `OUTLINE_JSON`.
 */
#define OUTLINE_PERIOD (15 * 1000)

/**
 * \def OUTLINE_JSON
 * The `g_data.outline_json`; relative to `"%TEMP%\\dump1090"`.
 */
#define OUTLINE_JSON "data\\outline.json"

/**
 * The function-pointers to lookup country/military status
 * of an aircraft.
 */
typedef const char *(*func_get_country) (uint32_t addr, bool get_short);
typedef bool        (*func_is_military) (uint32_t addr, const char **country);

/**
 * \def RANGEDIRS_BUCKETS
 * One bucket for every degree in circle
 */
#define RANGEDIRS_BUCKETS 360

/**
 * \def RANGEDIRS_IVAL
 * Intervals for `g_data.range_dirs[]`
 */
#define RANGEDIRS_IVALS   64

typedef struct dist_coords {
        float   lat;
        float   lon;
        float   distance;    /* in feet */
        int32_t alt;         /* in feet */
      } dist_coords;

/**
 * The private data used here.
 */
typedef struct aircraft_priv {
        char            *csv_file;           /**< Full path of default `AIRCRAFT_DATABASE_CSV` file */
        CSV_context      csv_ctx;            /**< CSV parser context */
        uint32_t         csv_len;            /**< The length of `g_data.csv_list` list */
        bool             csv_done;           /**< For `aircraft_load_CSV()` */
        aircraft_info   *csv_list;           /**< List of aircrafts sorted on address. From CSV-file only */
        aircraft_info   *csv_copy;           /**< Variables for `CSV_add_entry()` */
        aircraft_info   *csv_dest;
        aircraft_info   *csv_hi_end;
        aircraft_info    current_rec;        /**< Current work-record in `CSV_callback()` */
        aircraft_info    current_BIN;        /**< Current work-record in `BIN_lookup_entry()` */
        char            *sql_file;           /**< Equals `g_data.csv_file + ".sqlite"` */
        sqlite3         *sql_db;             /**< Sqlite instance of `g_data.sql_file` file */
        bool             sql_file_found;     /**< Does `g_data.sql_file` exist? */
        bool             test_mode;          /**< "--test aircraft" was specified */
        bool             test_done;          /**< Already done tests? */
        char            *zip_url;            /**< URL for OpenSky's `AIRCRAFT_DATABASE_URL` */
        func_get_country p_get_country;      /**< Func-pointer to a `aircraft_get_country()` function */
        func_is_military p_is_military;      /**< Func-pointer to a `aircraft_is_military()` function */
        int              a_sort;             /**< The column sort method for aircrafts in `--interactive` mode. >= 0 is ascending, < 0 descending */
        char            *icao_spec;          /**< A ICAO-filter was specified */
        mg_str           icao_filter;        /**< The `mg_str` of `icao_spec` */
        bool             icao_invert;        /**< Invert search for ICAO-filter? */
        bool             internal_error;     /**< Internal error in `aircraft_make_one_json()` */
        smartlist_t     *aircrafts;          /**< List of active aircrafts */

        /** Data for "/data/receiver.json"
         */
        uint64_t         json_interval;      /**< Client polling interval; 1 sec */
        int              json_history_next;  /**<  */
        mg_str           json_history [120]; /**<  */

        /** Data for `OUTLINE_JSON` file
         */
        bool             outline_enable;
        char            *outline_json;
        dist_coords      range_dirs [RANGEDIRS_IVALS] [RANGEDIRS_BUCKETS];  /* updated in track.c */

      } aircraft_priv;

static aircraft_priv g_data;

#if defined(USE_BIN_FILES)
  /*
   * Includes '$(OBJ_DIR)/gen_data.h' and `struct aircraft_record` etc.
   */
  #include "gen_data.h"
#endif

/**
 * \def DB_COLUMNS
 * The Sqlite columns we define.
 */
#define DB_COLUMNS "icao24,reg,manufacturer,type,callsign"
//                  |      |   |            |     |
//                  |      |   |            |     |__ == field 10: "operatorcallsign"
//                  |      |   |            |________ == field 8:  "icaoaircrafttype"
//                  |      |   |_____________________ == field 3:  "manufacturername"
//                  |      |_________________________ == field 1:  "registration"
//                  |________________________________ == field 0:  "icao24"

/**
 * \def DB_INSERT
 * The statement used when creating the Sqlite database
 */
#define DB_INSERT  "INSERT INTO aircrafts (" DB_COLUMNS ") VALUES"

/**
 * \def CSV_MAX_AGE
 * Max age to consider the CSV-file up-to-date. In seconds.
 */
#define CSV_MAX_AGE  (10 * 24 * 3600)

static bool sql_init (const char *what, int flags);
static bool sql_create (void);
static bool sql_open (void);
static bool sql_begin (void);
static bool sql_end (void);
static bool sql_add_entry (uint32_t num, const aircraft_info *ai);

static bool  aircraft_outline_generate (uint64_t now, const char *fname);
static bool  aircraft_outline_update (aircraft *a, uint64_t now);
static bool  is_helicopter_type (const char *type);
static const aircraft_info *CSV_lookup_entry (uint32_t addr);
static const aircraft_info *SQL_lookup_entry (uint32_t addr);
static void                *BIN_lookup_entry (uint32_t addr, bool convert);

/**
 * Internal error in `aircraft_make_one_json()`?
 */
bool aircraft_internal_error (void)
{
  return (g_data.internal_error);
}

/**
 * Lookup an aircraft in the BIN-file, CSV `g_data.csv_list`,
 * `g_data.airgrafts` cache or do a SQLite lookup.
 */
static const aircraft_info *aircraft_lookup (uint32_t addr, bool *from_sql, bool *from_bin)
{
  const aircraft_info *ai;
  const aircraft      *a;

  *from_sql = false;
  *from_bin = false;

#if defined(USE_BIN_FILES)
  ai = BIN_lookup_entry (addr, true);  /* convert this into an `aircraft_info` record first */
  if (ai)
  {
    Modes.stat.unique_aircrafts_BIN++;
    *from_bin = true;
    return (ai);
  }
#endif

  ai = CSV_lookup_entry (addr);
  if (ai)
     return (ai);

  a = aircraft_find (addr);
  if (a)
       ai = a->SQL;
  else ai = SQL_lookup_entry (addr);   /* do the `SELECT * FROM` */

  if (ai)
     *from_sql = true;

  return (ai);
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
static aircraft *aircraft_create (uint32_t addr, uint64_t now)
{
  aircraft            *a = calloc (sizeof(*a), 1);
  const aircraft_info *ai;
  bool                 from_sql, from_bin;

  if (!a)
     return (NULL);

  a->addr       = addr;
  a->seen_first = now;
  a->seen_last  = now;
  a->show       = A_SHOW_FIRST_TIME;

  ai = aircraft_lookup (addr, &from_sql, &from_bin);
  if (ai)
     a->is_helicopter = is_helicopter_type (ai->type);

  /* We really can't tell if it's unique since we keep no global list of that yet
   */
  Modes.stat.unique_aircrafts++;
  if (a->is_helicopter)
     Modes.stat.unique_helicopters++;

  if (from_sql)
  {
    /* Need to duplicate record from `sqlite3_exec()`.
     * Free it on `aircraft_exit (true)`.
     */
    a->SQL = malloc (sizeof(*a->SQL));
    if (a->SQL)
    {
      Modes.stat.unique_aircrafts_SQL++;
      memcpy (a->SQL, ai, sizeof(*a->SQL));
    }
  }
  else
  {
    Modes.stat.unique_aircrafts_CSV++;

    /* This points into the `g_data.csv_list` array.
     * No need to `free()`.
     */
    a->CSV = ai;
  }
  return (a);
}

/**
 * Return the aircraft with the specified ICAO address, or NULL
 * if we have no aircraft with this ICAO address.
 *
 * \param in addr  the specific ICAO address.
 */
aircraft *aircraft_find (uint32_t addr)
{
  aircraft *a;
  int       i, max = smartlist_len (g_data.aircrafts);

  for (i = 0; i < max; i++)
  {
    a = smartlist_get (g_data.aircrafts, i);
    if (a->addr == addr)
       return (a);
  }
  return (NULL);
}

/**
 * Find the aircraft with address `addr` or create a new one.
 */
static aircraft *aircraft_find_or_create (uint32_t addr, uint64_t now, bool *is_new)
{
  aircraft *a = aircraft_find (addr);

  *is_new = false;
  if (!a)
  {
    a = aircraft_create (addr, now);
    if (a)
    {
      smartlist_add (g_data.aircrafts, a);
      *is_new = true;
    }
  }
  return (a);
}

/**
 * Return the aircraft at index `idx`.
 */
aircraft *aircraft_get (int idx)
{
  if (!g_data.aircrafts)
     return (NULL);
  return smartlist_get (g_data.aircrafts, idx);
}

/**
 * Return the number of aircrafts we have now.
 */
int aircraft_len (void)
{
  if (!g_data.aircrafts)
     return (0);
  return smartlist_len (g_data.aircrafts);
}

/**
 * Return the number of valid aircrafts we have now.
 */
int aircraft_len_valid (void)
{
  int   i, valid, max = smartlist_len (g_data.aircrafts);
  const aircraft *a;

  for (i = valid = 0; i < max; i++)
  {
    a = smartlist_get (g_data.aircrafts, i);
    if (aircraft_valid(a))
        valid++;
  }
  return (valid);
}

/**
 * Valid data for aircraft in interactive mode?
 */
bool aircraft_valid (const aircraft *a)
{
  int  msgs  = a->messages;
  int  flags = a->mode_AC_flags;
  bool mode_AC     = (flags & MODEAC_MSG_FLAG);
  bool mode_A_only = ((flags & (MODEAC_MSG_MODES_HIT | MODEAC_MSG_MODEA_ONLY)) == MODEAC_MSG_MODEA_ONLY);
  bool mode_C_old  = (flags & (MODEAC_MSG_MODES_HIT | MODEAC_MSG_MODEC_OLD));

  bool valid = (!mode_AC    && msgs > 1) ||
               (mode_A_only && msgs > 4) ||
               (!mode_C_old && msgs > 127);

  if (a->addr & MODES_NON_ICAO_ADDRESS)
     valid = false;

#if 1
  if (!Modes.sbs_in && !memcmp(a->call_sign, "TEST1234", 8))
     LOG_FILEONLY ("TEST1234: valid: %d, mode_AC: 0x%08X, msgs: %d\n", valid, mode_AC, msgs);
#endif

  return (valid);
}

static const struct search_list MODES_AC_flags[] = {
     { MODES_ACFLAGS_ALTITUDE_VALID,     "ALTITUDE_VALID"     },
     { MODES_ACFLAGS_AOG,                "AOG"                },
     { MODES_ACFLAGS_AOG_VALID,          "AOG_VALID"          },
     { MODES_ACFLAGS_LLEVEN_VALID,       "LLEVEN_VALID"       },
     { MODES_ACFLAGS_LLODD_VALID,        "LLODD_VALID"        },
     { MODES_ACFLAGS_CALLSIGN_VALID,     "CALLSIGN_VALID"     },
     { MODES_ACFLAGS_LATLON_VALID,       "LATLON_VALID"       },
     { MODES_ACFLAGS_ALTITUDE_HAE_VALID, "ALTITUDE_HAE_VALID" },
     { MODES_ACFLAGS_HAE_DELTA_VALID,    "HAE_DELTA_VALID"    },
     { MODES_ACFLAGS_HEADING_VALID,      "HEADING_VALID"      },
     { MODES_ACFLAGS_SQUAWK_VALID,       "SQUAWK_VALID"       },
     { MODES_ACFLAGS_SPEED_VALID,        "SPEED_VALID"        },
     { MODES_ACFLAGS_CATEGORY_VALID,     "CATEGORY_VALID"     },
     { MODES_ACFLAGS_FROM_MLAT,          "FROM_MLAT"          },
     { MODES_ACFLAGS_FROM_TISB,          "FROM_TISB"          },
     { MODES_ACFLAGS_VERTRATE_VALID,     "VERTRATE_VALID"     },
     { MODES_ACFLAGS_REL_CPR_USED,       "REL_CPR_USED"       },
     { MODES_ACFLAGS_LATLON_REL_OK,      "LATLON_REL_OK"      },
     { MODES_ACFLAGS_NSEWSPD_VALID,      "NSEWSPD_VALID"      },
     { MODES_ACFLAGS_FS_VALID,           "FS_VALID"           },
     { MODES_ACFLAGS_EWSPEED_VALID,      "EWSPEED_VALID"      },
     { MODES_ACFLAGS_NSSPEED_VALID,      "NSSPEED_VALID"      },
     { MODES_ACFLAGS_LLBOTH_VALID,       "LLBOTH_VALID"       },
     { MODES_ACFLAGS_LLEITHER_VALID,     "LLEITHER_VALID"     }
   };

static const struct search_list MSG_flags[] = {
     { MODEAC_MSG_FLAG,       "MSG_FLAG"       },
     { MODEAC_MSG_MODES_HIT,  "MSG_MODES_HIT"  },
     { MODEAC_MSG_MODEA_HIT,  "MSG_MODEA_HIT"  },
     { MODEAC_MSG_MODEC_HIT,  "MSG_MODEC_HIT"  },
     { MODEAC_MSG_MODEA_ONLY, "MSG_MODEA_ONLY" },
     { MODEAC_MSG_MODEC_OLD,  "MSG_MODEC_OLD"  }
   };

/**
 * Decode `aircraft::AC_flags` to figure out which bits make a plane
 * valid or not for interactive mode.
 */
const char *aircraft_AC_flags (enum AIRCRAFT_FLAGS flags)
{
  return flags_decode ((DWORD)flags, MODES_AC_flags, DIM(MODES_AC_flags));
}

/**
 * Decode `aircraft::mode_AC_flags`
 */
const char *aircraft_mode_AC_flags (enum MODEAC_FLAGS flags)
{
  return flags_decode ((DWORD)flags, MSG_flags, DIM(MSG_flags));
}

/**
 * All compare function fallback to `compare_on_icao()`.
 */
static int compare_on_icao (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;

  if (a->addr < b->addr)
     return (-1);
  if (a->addr > b->addr)
     return (1);
  return (0);
}

static int compare_on_altitude (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;

  if (a->altitude == b->altitude)
     return compare_on_icao (_a, _b);
  return (a->altitude - b->altitude);
}

static int compare_on_distance (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;

  if (a->distance == b->distance)
     return compare_on_icao (_a, _b);
  return (a->distance - b->distance);
}

static int compare_on_seen (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;

  if (a->seen_last > b->seen_last)
     return (-1);
  if (a->seen_last < b->seen_last)
     return (1);
  return compare_on_icao (_a, _b);
}

static int compare_on_speed (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;

  if (a->speed == b->speed)
     return compare_on_icao (_a, _b);
  return (a->speed - b->speed);
}

static int compare_on_messages (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;

  if (a->messages > b->messages)
     return (-1);
  if (a->messages < b->messages)
     return (1);
  return compare_on_icao (_a, _b);
}

static int compare_on_callsign (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;
  const char     *a_p = a->call_sign;
  const char     *b_p = b->call_sign;
  int   rc;

  while (*a_p && isspace(*a_p))
        a_p++;
  while (*b_p && isspace(*b_p))
        b_p++;

  rc = strcmp (a_p, b_p);
  if (!*a_p && !*b_p)
     rc = compare_on_icao (_a, _b);
  return (rc);
}

static int compare_on_country (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;
  const char *a_short = (*g_data.p_get_country) (a->addr, true);
  const char *b_short = (*g_data.p_get_country) (b->addr, true);
  int   rc;

  if (!a_short)
     a_short = "--";
  if (!b_short)
     b_short = "--";

  rc = strcmp (a_short, b_short);
  if (rc == 0)
     rc = compare_on_icao (_a, _b);
  return (rc);
}

static int compare_on_regnum (const void **_a, const void **_b)
{
  const aircraft *a = *_a;
  const aircraft *b = *_b;
  const char     *a_reg_num = "-";
  const char     *b_reg_num = "-";
  int   rc;

  if (a->SQL && a->SQL->reg_num[0])
     a_reg_num = a->SQL->reg_num;
  else if (a->CSV && a->CSV->reg_num[0])
     a_reg_num = a->CSV->reg_num;

  if (b->SQL && b->SQL->reg_num[0])
     b_reg_num = b->SQL->reg_num;
  else if (b->CSV && b->CSV->reg_num[0])
     b_reg_num = b->CSV->reg_num;

  rc = strcmp (a_reg_num, b_reg_num);
  if (rc == 0)
     rc = compare_on_icao (_a, _b);
  return (rc);
}

#define ADD_VALUE(v)  { (DWORD)v, #v }

static const search_list sort_names[] = {
                         ADD_VALUE (INTERACTIVE_SORT_NONE),
                         ADD_VALUE (INTERACTIVE_SORT_ICAO),
                         ADD_VALUE (INTERACTIVE_SORT_CALLSIGN),
                         ADD_VALUE (INTERACTIVE_SORT_REGNUM),
                         ADD_VALUE (INTERACTIVE_SORT_COUNTRY),
                         ADD_VALUE (INTERACTIVE_SORT_DEP_DEST),
                         ADD_VALUE (INTERACTIVE_SORT_ALTITUDE),
                         ADD_VALUE (INTERACTIVE_SORT_SPEED),
                         ADD_VALUE (INTERACTIVE_SORT_DISTANCE),
                         ADD_VALUE (INTERACTIVE_SORT_MESSAGES),
                         ADD_VALUE (INTERACTIVE_SORT_SEEN)
                       };

static const search_list sort_values[] = {
                       { INTERACTIVE_SORT_ICAO,     "addr" },
                       { INTERACTIVE_SORT_ICAO,     "icao" },
                       { INTERACTIVE_SORT_CALLSIGN, "callsign" },
                       { INTERACTIVE_SORT_CALLSIGN, "call-sign" },
                       { INTERACTIVE_SORT_REGNUM,   "regnum" },
                       { INTERACTIVE_SORT_COUNTRY,  "country" },
                       { INTERACTIVE_SORT_DEP_DEST, "dep-dest" },
                       { INTERACTIVE_SORT_DEP_DEST, "route" },
                       { INTERACTIVE_SORT_ALTITUDE, "alt" },
                       { INTERACTIVE_SORT_ALTITUDE, "altitude" },
                       { INTERACTIVE_SORT_SPEED,    "speed" },
                       { INTERACTIVE_SORT_DISTANCE, "dist" },
                       { INTERACTIVE_SORT_DISTANCE, "distance" },
                       { INTERACTIVE_SORT_MESSAGES, "msg" },
                       { INTERACTIVE_SORT_MESSAGES, "messages" },
                       { INTERACTIVE_SORT_SEEN,     "age" },
                       { INTERACTIVE_SORT_SEEN,     "seen" },
                     };

const char *aircraft_sort_name (int s)
{
  static char  buf [100];
  char        *p = buf, *end = p + sizeof(buf);
  const  char *name = search_list_name (abs(s), sort_names, DIM(sort_names));

  if (name)
       p += snprintf (p, end - p, "%s", name);
  else p += snprintf (p, end - p, "%d", s);
  snprintf (p, end - p, ", %s", s < 0 ? "descending" : "ascending");
  return (buf);
}

/**
 * Return current sort value
 */
a_sort_t aircraft_get_sort (void)
{
  return (g_data.a_sort);
}

/**
 * Called from interactive.c to change the `g_data.a_sort` value.
 */
a_sort_t aircraft_do_sort (int s)
{
  int  num = smartlist_len (g_data.aircrafts);
  bool reverse = (s < 0);

  if (num <= 1 || abs(s) == INTERACTIVE_SORT_NONE)   /* no point */
     return (g_data.a_sort);

  switch (abs(s))
  {
    case INTERACTIVE_SORT_CALLSIGN:
         smartlist_sort (g_data.aircrafts, compare_on_callsign, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_COUNTRY:
         smartlist_sort (g_data.aircrafts, compare_on_country, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_ICAO:
         smartlist_sort (g_data.aircrafts, compare_on_icao, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_ALTITUDE:
         smartlist_sort (g_data.aircrafts, compare_on_altitude, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_DISTANCE:
         smartlist_sort (g_data.aircrafts, compare_on_distance, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_REGNUM:
         smartlist_sort (g_data.aircrafts, compare_on_regnum, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_SPEED:
         smartlist_sort (g_data.aircrafts, compare_on_speed, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_SEEN:
         smartlist_sort (g_data.aircrafts, compare_on_seen, reverse);
         g_data.a_sort = s;
         break;
    case INTERACTIVE_SORT_MESSAGES:
         smartlist_sort (g_data.aircrafts, compare_on_messages, reverse);
         g_data.a_sort = s;
         break;
    default:
         LOG_FILEONLY ("Unimplemented sort-method '%s'.\n", aircraft_sort_name(s));
  }
  return (g_data.a_sort);
}

/**
 * Config-callback to set the `g_data.a_sort` method.
 */
bool aircraft_set_sort (const char *arg)
{
  DWORD value = search_list_value (arg, sort_values, DIM(sort_values));

  if (value == (DWORD)-1)
  {
    LOG_STDERR ("Illegal 'sort' method '%s'. Use: 'call-sign', 'country', 'icao', "
                "'altitude', 'dep-dest', 'distance', 'messages', 'seen', 'speed' or 'regnum'\n", arg);
    return (false);
  }
  g_data.a_sort = (enum a_sort_t) value;
  return (true);
}

/**
 * Add an aircraft record to `g_data.csv_list`.
 */
static int CSV_add_entry (const aircraft_info *ai)
{
  /* Not a valid ICAO address. Parse error?
   */
  if (ai->addr == 0 || (ai->addr & MODES_NON_ICAO_ADDRESS))
     return (1);

  if (!g_data.csv_list || /* Create the initial buffer */
      g_data.csv_dest == g_data.csv_hi_end - 1)
  {
    size_t new_num = 10000 + g_data.csv_len;

    g_data.csv_copy   = realloc (g_data.csv_list, sizeof(*ai) * new_num);
    g_data.csv_dest   = g_data.csv_copy + g_data.csv_len;
    g_data.csv_hi_end = g_data.csv_copy + new_num;
  }

  if (!g_data.csv_copy)
     return (0);

  g_data.csv_list = g_data.csv_copy;
  assert (g_data.csv_dest < g_data.csv_hi_end);
  memcpy (g_data.csv_dest, ai, sizeof(*ai));
  g_data.csv_len++;
  g_data.csv_dest = g_data.csv_copy + g_data.csv_len;
  return (1);
}

/**
 * The compare function for `qsort()` and `bsearch()`.
 */
static int CSV_compare_on_addr (const void *a, const void *b)
{
  const aircraft_info *ai1 = (const aircraft_info*) a;
  const aircraft_info *ai2 = (const aircraft_info*) b;

  if (ai1->addr < ai2->addr)
     return (-1);
  if (ai1->addr > ai2->addr)
     return (1);
  return (0);
}

/**
 * Do a binary search for an aircraft in `g_data.csv_list`.
 */
static const aircraft_info *CSV_lookup_entry (uint32_t addr)
{
  aircraft_info key = { addr, "" };

  if (!g_data.csv_list)
     return (NULL);
  return bsearch (&key, g_data.csv_list, g_data.csv_len,
                  sizeof(*g_data.csv_list), CSV_compare_on_addr);
}

#if defined(USE_BIN_FILES)
static bool aircraft_init_BIN (void)
{
  BIN_header *hdr;
  struct stat st;
  time_t      created;
  const char *fname;
  mg_str      mem;
  bool        exist;
  bool        truncated = false;

  Modes.bin.aircrafts_bin = mg_mprintf ("%s\\%s", Modes.results_dir, AIRCRAFT_BIN);
  fname = Modes.bin.aircrafts_bin;
  memset (&st, '\0', sizeof(st));
  exist = (stat(fname, &st) == 0);

  if (exist)
     truncated = (st.st_size < sizeof(BIN_header));

  if (!exist)
     LOG_STDERR ("file: '%s' is missing.\n", fname);

  if (truncated)
     LOG_STDERR ("file: '%s' is truncated.\n", fname);

  if (!exist || truncated)
     return (false);

  mem = mg_file_read (&mg_fs_posix, fname);
  if (!mem.buf)
  {
    LOG_STDERR ("Failed to open %s, errno: %d/%s\n", fname, errno, strerror(errno));
    return (false);
  }

  hdr = (BIN_header*) mem.buf;
  created = hdr->created;

  /**
   * \todo Remember when it was created in case we need to reload a newer
   *       version created while we were running.
   */
  Modes.bin.aircrafts_created = created;

  if (Modes.debug & DEBUG_GENERAL)
     puts ("");
  TRACE ("bin_file:   %s\n",    fname);
  TRACE ("bin_marker: %.*s\n",  (int)sizeof(hdr->bin_marker), hdr->bin_marker);
  TRACE ("created:    %.24s\n", ctime(&created));
  TRACE ("rec_len:    %u\n",    hdr->rec_len);
  TRACE ("rec_num:    %u\n\n",  hdr->rec_num);

  Modes.bin.aircrafts_records_num = hdr->rec_num;
  Modes.bin.aircrafts_records = hdr;
  return (true);
}

/*
 * Convert from `aircraft_record` to `aircraft_info`:
 *
 * typedef struct aircraft_info {
 *         uint32_t addr;
 *         char     reg_num  [10];
 *         char     manufact [30];
 *         char     type     [10];
 *         char     call_sign[20];
 *       } aircraft_info;
 *
 * typedef struct aircraft_record {
 *         char icao_addr [6];
 *         char regist   [10];
 *         char manuf    [10];
 *         char model    [40];
 *       } aircraft_record;
 */
static void *aircraft_convert_BIN (const aircraft_record *a)
{
  memset (&g_data.current_BIN, '\0', sizeof(g_data.current_BIN));

  g_data.current_BIN.addr = mg_unhexn (a->icao_addr, sizeof(a->icao_addr));
  strncpy (g_data.current_BIN.reg_num,  a->regist, sizeof(g_data.current_BIN.reg_num)-1);
  strncpy (g_data.current_BIN.manufact, a->manuf,  sizeof(g_data.current_BIN.manufact)-1);
  strncpy (g_data.current_BIN.type,     a->manuf,  sizeof(g_data.current_BIN.type)-1);
  return (&g_data.current_BIN);
}

static int aircraft_compare (const void *_a, const void *_b)
{
  aircraft_record *a = (aircraft_record*) _a;
  aircraft_record *b = (aircraft_record*) _b;
  uint32_t a_addr = mg_unhexn (a->icao_addr, sizeof(a->icao_addr));
  uint32_t b_addr = mg_unhexn (b->icao_addr, sizeof(b->icao_addr));

  return (int) (a_addr - b_addr);
}

static void *BIN_lookup_entry (uint32_t addr, bool convert)
{
  aircraft_record  key;
  aircraft_record *a = NULL;

  snprintf (key.icao_addr, sizeof(key.icao_addr), "%06X", addr);
  if (Modes.bin.aircrafts_records_num > 0)
     a = bsearch (&key, Modes.bin.aircrafts_records, Modes.bin.aircrafts_records_num, sizeof(key), aircraft_compare);

  if (a && convert)
     return aircraft_convert_BIN (a);

  return (a);
}
#endif  /* USE_BIN_FILES */

static void free_CSV_BIN_records (void)
{
  free (g_data.csv_list);
  g_data.csv_list = NULL;
  g_data.csv_len = 0;

#if defined(USE_BIN_FILES)
  free (Modes.bin.aircrafts_records);
  Modes.bin.aircrafts_records = NULL;
  Modes.bin.aircrafts_records_num = 0;
#endif
}

static uint32_t get_address (unsigned rec_num)
{
  const aircraft_info *a1;

#if defined(USE_BIN_FILES)
  if (Modes.bin.aircrafts_records && 0)
  {
    const aircraft_record *a2;

    assert (rec_num < Modes.bin.aircrafts_records_num);
    a2 = (const aircraft_record*) Modes.bin.aircrafts_records + rec_num;
    return mg_unhexn (a2->icao_addr, sizeof(a2->icao_addr));
  }
#endif

  assert (rec_num < g_data.csv_len);
  a1 = g_data.csv_list + rec_num;
  return (a1->addr);
}

/**
 * Do a simple test on the `g_data.csv_list`.
 *
 * Also, if `g_data.sql_file_found == true`, compare the lookup speed
 * of Sqlite3 compared to our `bsearch()` lookup used in `CSV_lookup_entry()`.
 */
static void aircraft_test_1 (void)
{
  const char  *country;
  unsigned     i, num_ok;
  static const aircraft_info ai_tests [] = {
               { 0xAA3496, "N757FQ",  "Cessna", "" },
               { 0xAB34DE, "N821DA",  "Beech",  "" },
               { 0x800737, "VT-ANQ",  "Boeing", "" },
               { 0xA713D5, "N555UW",  "Piper",  "" },
               { 0x3532C1, "T.23-01", "AIRBUS", "" },  /* callsign: AIRMIL, Spain */
               { 0x00A78C, "ZS-OYT",  "Bell", "H1P" }  /* test for a Helicopter */
             };
  const aircraft_info *ai = ai_tests + 0;
  char         sql_file    [MAX_PATH] = { "" };
  char         blocks_file [MAX_PATH] = { "" };
  bool         heli_found = false;
  const char  *heli_type  = "?";

  if (g_data.sql_file_found)
     snprintf (sql_file, sizeof(sql_file),
               "\n                   %s", g_data.sql_file);

  if (g_data.p_is_military != aircraft_is_military)
     snprintf (blocks_file, sizeof(blocks_file),
                "\n                   %s", "$(OBJ_DIR)/gen-code-blocks.c");

  printf ("\n%s(): Checking %zu fixed records against:\n"
          "                   %s%s%s\n",
          __FUNCTION__, DIM(ai_tests), g_data.csv_file, sql_file, blocks_file);

  for (i = num_ok = 0; i < DIM(ai_tests); i++, ai++)
  {
    const aircraft_info *a_CSV, *a_SQL;
    const char *reg_num   = "?";
    const char *manufact  = "?";
    const char *type      = "?";
    const char *type2     = NULL;

    a_CSV = CSV_lookup_entry (ai->addr);
    a_SQL = SQL_lookup_entry (ai->addr);

    if (a_CSV)
    {
      if (a_CSV->manufact[0])
         manufact = a_CSV->manufact;
      if (a_CSV->type[0])
         type = a_CSV->type;
      if (a_CSV->reg_num[0])
      {
        reg_num = a_CSV->reg_num;
        num_ok++;
      }
    }
    else if (a_SQL)
    {
      if (a_SQL->manufact[0])
         manufact = a_SQL->manufact;
      if (a_SQL->type[0])
         type = a_SQL->type;
      if (a_SQL->reg_num[0])
      {
        reg_num = a_SQL->reg_num;
        num_ok++;
      }
    }

    if (ai->type[0] && aircraft_is_helicopter(ai->addr, &type2))
    {
      heli_found = true;
      heli_type  = type2;
    }

    country = (*g_data.p_get_country) (ai->addr, false);
    printf ("  addr: 0x%06X, reg-num: %-8s manufact: %-20s type: %-4s country: %-30s %s\n",
            ai->addr, reg_num, manufact, type, country ? country : "?",
            (*g_data.p_is_military)(ai->addr, NULL) ? "Military" : "");
  }
  printf ("%3u OKAY. heli_found: %d -> %s\n", num_ok, heli_found, heli_type);
  printf ("%3u FAIL\n", i - num_ok);
}

/**
 * As above, but if `g_data.sql_file_found == true`, compare the lookup speed
 * of Sqlite3 compared to our `bsearch()` lookup used in `CSV_lookup_entry()`.
 *
 * And with `USE_BIN_FILES`, compare the speed of `bsearch()` lookup used
 * in `BIN_lookup_entry()`. This latter step assumes all ICAO addresses are
 * found in all 3 sources. But this is not the case.
 */
static void aircraft_test_2 (unsigned max_num)
{
  unsigned i;
  int      rec_width = 6;
  char     sql_file [MAX_PATH] = { "" };
  char     bin_file [MAX_PATH] = { "" };

#if defined(USE_BIN_FILES)
  if (Modes.bin.aircrafts_records)
     snprintf (bin_file, sizeof(bin_file), "\n                   %s",
               Modes.bin.aircrafts_bin);
#endif

  if (max_num < 100000)
     rec_width = 5;
  if (max_num < 10000)
     rec_width = 4;
  if (max_num < 1000)
     rec_width = 3;

  if (g_data.sql_file_found)
     snprintf (sql_file, sizeof(sql_file), "\n                   %s", g_data.sql_file);

  if (max_num == 0)
  {
    printf ("  cannot do this with empty list(s)\n");
    return;
  }

  printf ("\n%s(): Checking 5 random records in:\n"
          "                   %s%s%s\n"
          "                   max_num: 0 - %u:\n",
          __FUNCTION__, g_data.csv_file, sql_file, bin_file, max_num);

  for (i = 0; i < 5; i++)
  {
    const aircraft_info *a_CSV, *a_SQL;
    unsigned rec_num = random_range (0, max_num - 1);
    double   usec    = get_usec_now();
    uint32_t addr    = get_address (rec_num);

    a_CSV = CSV_lookup_entry (addr);
    usec  = get_usec_now() - usec;

    printf ("  CSV:  %*u: addr: 0x%06X, reg-num: %-8s  manufact: %-20.20s callsign: %-10.10s type: %-4s %6.0f usec\n",
            rec_width, rec_num, addr,
            (a_CSV && a_CSV->reg_num[0])   ? a_CSV->reg_num   : "-",
            (a_CSV && a_CSV->manufact[0])  ? a_CSV->manufact  : "-",
            (a_CSV && a_CSV->call_sign[0]) ? a_CSV->call_sign : "-",
            (a_CSV && a_CSV->type[0])      ? a_CSV->type      : "-",
            usec);

    if (g_data.sql_file_found)
    {
      usec  = get_usec_now();
      a_SQL = SQL_lookup_entry (addr);
      usec  = get_usec_now() - usec;

      printf ("  SQL:  %*s  addr: 0x%06X, reg-num: %-8s  manufact: %-20.20s callsign: %-10.10s type: %-4s %6.0f usec\n",
              rec_width, "", addr,
              (a_SQL && a_SQL->reg_num[0])   ? a_SQL->reg_num   : "-",
              (a_SQL && a_SQL->manufact[0])  ? a_SQL->manufact  : "-",
              (a_SQL && a_SQL->call_sign[0]) ? a_SQL->call_sign : "-",
              (a_SQL && a_SQL->type[0])      ? a_SQL->type      : "-",
              usec);
    }

#if defined(USE_BIN_FILES)
    {
      const aircraft_record *a_BIN;

      /* Since the 'a_BIN' text-records may NOT be 0-terminated.
       */
      int r_size = sizeof(a_BIN->regist) - 1;
      int m_size = sizeof(a_BIN->manuf) - 1;

      usec  = get_usec_now();
      a_BIN = BIN_lookup_entry (addr, false);
      usec = get_usec_now() - usec;

      printf ("  BIN1: %*s  addr: 0x%06X, reg-num: %-*.*s manufact: %-*.*s"
              "            model:    %-20.20s  %6.0f usec\n",
              rec_width, "", addr,
              r_size, r_size, (a_BIN && a_BIN->regist[0]) ? a_BIN->regist : "-",
              m_size, m_size, (a_BIN && a_BIN->manuf[0])  ? a_BIN->manuf  : "-",
              (a_BIN && a_BIN->model[0]) ? a_BIN->model : "-", usec);

      if (a_BIN)
      {
        const aircraft_info *a = aircraft_convert_BIN (a_BIN);

        printf ("  BIN2: %*s  addr: 0x%06X, reg-num: %-*.*s manufact: %-*.*s"
                "                      type: %-4s\n",
                rec_width, "", addr,
                r_size,    r_size,    a->reg_num,
                m_size+11, m_size+11, a->manufact, a->type);
      }
    }
#endif
  }
}

/**
 * Generate a single json .TXT-file (binary mode) and run
 * `jq.exe < %TEMP%\dump1090\filename > NUL` to verify it.
 */
static void aircraft_dump_json (char *data, const char *filename)
{
  FILE  *f;
  size_t sz = data ? strlen(data) : 0;
  int    rc;
  char   jq_cmd   [MAX_PATH + 20];
  char   tmp_file [MAX_PATH];

  snprintf (tmp_file, sizeof(tmp_file), "%s\\%s", Modes.tmp_dir, filename);
  printf ("  Dumping %d aircrafts (%zu bytes) to '%s'\n", aircraft_len(), sz, tmp_file);

  if (!data)
  {
    printf ("  No 'data'! errno: %d/%s\n\n", errno, strerror(errno));
    return;
  }

  f = fopen (tmp_file, "w+");
  if (!f)
  {
    printf ("  Creating %s failed; errno: %d/%s\n\n", tmp_file, errno, strerror(errno));
    free (data);
    return;
  }

  fwrite (data, 1, strlen(data), f);
  free (data);
  fclose (f);
  snprintf (jq_cmd, sizeof(jq_cmd), "jq.exe < %s > NUL", tmp_file);
  errno = 0;
  rc = system (jq_cmd);
  if (rc == 0)
       printf ("  File %s OK.\n\n", tmp_file);
  else if (rc < 0)
       printf ("  File %s failed; errno: %d/%s\n\n", tmp_file, errno, strerror(errno));
  else printf ("  Command '%s' failed; rc: %d.\n\n", jq_cmd, rc);
}

/**
 * Generate some json-files to test the `aircraft_make_json()`
 * function with a large number of aircrafts.
 * The data-content does not matter.
 */
static void aircraft_test_3 (unsigned max_num)
{
  unsigned i;
  size_t unused;

  printf ("\n%s(): Generate and check JSON-data:\n", __FUNCTION__);

  if (!Modes.home_pos_ok)
  {
    printf ("Setting home_pos to London.\n");
    Modes.home_pos.lat = 51.5285578;
    Modes.home_pos.lon = -0.2420247;
  }

  /* Create a list of aircrafts with a position around our home-position
   */
  for (i = 0; i < max_num; i++)
  {
    bool      is_new;
    aircraft *a = aircraft_find_or_create (0x470000 + i, MSEC_TIME(), &is_new);

    if (!a)
       break;

    assert (is_new);
    strcpy (a->call_sign, "test");
    a->position = Modes.home_pos;
    a->position.lat += random_range2 (-2, 2);
    a->position.lon += random_range2 (-2, 2);
    a->altitude = random_range (0, 10000);
    a->heading  = random_range2 (-180, 180);
    a->speed    = 1.852 * random_range (100, 1000);
    a->identity = 0x002;
    a->messages = 2;
    a->category = 0x2A;
    a->AC_flags = (MODES_ACFLAGS_ALTITUDE_VALID |
                   MODES_ACFLAGS_CALLSIGN_VALID |
                   MODES_ACFLAGS_LATLON_VALID   |
                   MODES_ACFLAGS_HEADING_VALID  |
                   MODES_ACFLAGS_SQUAWK_VALID   |
                   MODES_ACFLAGS_SPEED_VALID    |
                   MODES_ACFLAGS_CATEGORY_VALID |
                   MODES_ACFLAGS_LLBOTH_VALID   |
                   MODES_ACFLAGS_LLEITHER_VALID);

    /* for 'aircraft_test_4()':
     */
    for (int j = 0; j < RANGEDIRS_BUCKETS; j++)
    {
      dist_coords rec;

      rec.lat = a->position.lat;
      rec.lon = a->position.lon;
      rec.alt = a->altitude;
      rec.distance = geo_great_circle_dist (&Modes.home_pos, &a->position) * 3.2808;
      g_data.range_dirs [0][j] = rec;
    }
  }

  Modes.stat.messages_total = max_num;

  aircraft_dump_json (aircraft_make_json(false, &unused), "json-1.txt");
  aircraft_dump_json (aircraft_make_json(true, &unused), "json-2.txt");

  smartlist_t *save = g_data.aircrafts;
  smartlist_t *empty = smartlist_new();

  /* Test empty json-data too.
   */
  g_data.aircrafts = empty;
  aircraft_dump_json (aircraft_make_json(false, &unused), "json-3.txt");
  aircraft_dump_json (aircraft_make_json(true, &unused), "json-4.txt");

  smartlist_free (empty);
  g_data.aircrafts = save;
}

#define JS_FETCH(_file)                                                                 \
        "    fetch (" #_file ")",                                                       \
        "      .then (response => response.json())",                                    \
        "      .then (data => {",                                                       \
        "          // Extract the points array",                                        \
        "          const points = data.actualRange.last24h.points;",                    \
        "          // Create a polyline from all points",                               \
        "          const coordinates = points.map (point => [ point[0], point[1] ]);",  \
        "          L.polyline (coordinates, {color: \"blue\", weight: 2}).addTo(map);", \
        "      });"

/**
 * \todo Make this configurable
 */
#define JS_HOMEPOS  "60.30, 5.30"
#define JS_ZOOM     "6"

static const char *show_outline_html[] = {
  "<!doctype html>",
  "<html>",
  "<head>",
  "  <title>Flight Range Map</title>",
  "  <link rel=\"stylesheet\" href=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css\" />",
  "  <style>",
  "    html, body {",
  "      margin:  0;",
  "      padding: 0;",
  "      height: 100%;",
  "      width:  100%;",
  "    }",
  "    #map {",
  "      height: 100vh;",
  "      width:  100%;",
  "    }",
  "  </style>",
  "</head>",
  "",
  "<body>",
  "  <div id=\"map\"></div>",
  "  <script src=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.js\"></script>",
  "  <script>",
  "    // Initialize the map",
  "    const map = L.map(\"map\").setView([ " JS_HOMEPOS "], " JS_ZOOM "); /* Bergen,  Norway */",
  "    // Add OpenStreetMap tiles",
  "    L.tileLayer (\"https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png\", {",
  "        attribution: \"(c) OpenStreetMap contributors\"",
  "    }).addTo(map);",
  "",
  JS_FETCH ("our_outline.json"),
  JS_FETCH ("readsb_outline.json"),
  "    </script>",
  "</body>",
  "</html>",
  NULL
};

/*
 * These JSON data was generated by "readsb".
 * `aircraft_test_4()` creates `Modes.tmp_dir\\outline-test\\readsb_outline.json`
 * from this array.
 *
 * It should compare the result of the `Modes.tmp_dir\\outline-test\\readsb_outline.json` to our
 * semi-randomly generated at `Modes.tmp_dir\\outline-test\\our_outline.json`.
 */
static const char *readsb_outline_json[] = {
  "{ \"actualRange\": { \"last24h\": { \"points\": [",
  "[61.1661,5.3151,36000],",  "[61.1625,5.3354,36000],",  "[61.1110,5.3682,40000],",  "[61.1516,5.3960,36000],",
  "[61.0870,5.4059,40000],",  "[61.0719,5.4296,40000],",  "[60.6894,5.3827,16775],",  "[60.6704,5.3941,29000],",
  "[60.6790,5.4187,8400],",   "[60.9932,5.5388,23875],",  "[60.9653,5.5361,23450],",  "[60.8519,5.5253,21475],",
  "[60.6525,5.4584,29000],",  "[60.6502,5.4669,29000],",  "[60.5585,5.4358,14975],",  "[60.6438,5.4898,29000],",
  "[60.6420,5.4961,29000],",  "[60.4974,5.4222,41000],",  "[60.6352,5.5207,29000],",  "[60.6307,5.5369,29000],",
  "[60.4889,5.4381,35000],",  "[60.4863,5.4432,35000],",  "[60.6225,5.5663,29000],",  "[60.4927,5.4681,5700],",
  "[60.7808,5.7327,34400],",  "[60.6146,5.5942,29000],",  "[60.5564,5.5504,15600],",  "[60.5121,5.5187,14425],",
  "[60.4682,5.4782,41000],",  "[60.4650,5.4845,41000],",  "[60.6491,5.7067,35500],",  "[60.4877,5.5286,6350],",
  "[60.4688,5.5153,7400],",   "[60.4822,5.5408,6475],",   "[60.4536,5.5057,35000],",  "[60.4512,5.5101,35000],",
  "[60.4482,5.5160,35000],",  "[60.4628,5.5483,15300],",  "[60.4439,5.5247,41000],",  "[60.4422,5.5274,35000],",
  "[60.4809,5.6098,7250],",   "[60.4685,5.5960,6075],",   "[60.4568,5.5815,8425],",   "[60.4554,5.5951,7725],",
  "[60.4302,5.5507,41000],",  "[60.5534,5.8118,29000],",  "[60.5493,5.8264,29000],",  "[60.4503,5.6250,7975],",
  "[60.4608,5.6544,6600],",   "[60.4475,5.6370,9125],",   "[60.4370,5.6227,8875],",   "[60.4436,5.6595,9425],",
  "[60.4131,5.5833,41000],",  "[60.4406,5.6775,9650],",   "[60.4615,5.7433,8425],",   "[60.4386,5.6952,8550],",
  "[60.4366,5.7067,8650],",   "[60.4647,5.8013,9325],",   "[60.4343,5.7202,8750],",   "[60.4611,5.8436,9800],",
  "[60.6379,6.4851,13750],",  "[60.4819,5.9538,16050],",  "[60.4448,5.8371,13975],",  "[60.4805,6.0182,16900],",
  "[60.4782,6.0522,17300],",  "[60.4776,6.0628,17425],",  "[60.4174,5.8185,9725],",   "[60.3250,5.4003,40000],",
  "[60.4260,5.9258,8575],",   "[60.4228,5.9501,8725],",   "[60.3263,5.4259,4850],",   "[60.3365,5.4884,39000],",
  "[60.4013,5.9144,9925],",   "[60.3983,5.9199,11650],",  "[60.3840,5.8681,11925],",  "[60.3896,5.9830,10375],",
  "[60.3777,5.8977,12300],",  "[60.3962,6.1534,10800],",  "[60.3948,6.1641,10925],",  "[60.3907,6.2080,19300],",
  "[60.3713,6.0821,14800],",  "[60.3573,5.9912,13450],",  "[60.3610,6.1015,13300],",  "[60.3652,6.4673,29000],",
  "[60.3474,6.1571,14025],",  "[60.3521,6.5121,29000],",  "[60.3347,6.2931,17475],",  "[60.3330,6.5778,29000],",
  "[60.3178,6.6295,29000],",  "[60.3046,6.4642,19525],",  "[60.2984,6.6956,29000],",  "[60.2896,6.7253,29000],",
  "[60.2783,6.6116,20025],",  "[60.2877,5.8716,28000],",  "[60.2906,5.7413,15775],",  "[60.2893,5.6637,11950],",
  "[60.2865,5.6607,10875],",  "[60.2836,5.6454,10725],",  "[60.2792,5.6674,12050],",  "[60.2765,5.6739,14450],",
  "[60.2321,6.1039,14850],",  "[60.2731,5.6388,10825],",  "[60.2350,5.9492,13325],",  "[60.2665,5.6260,13625],",
  "[60.2364,5.8537,12275],",  "[60.2366,5.7997,11675],",  "[60.2366,5.7869,11500],",  "[60.2364,5.7512,11100],",
  "[60.2598,5.5738,9925],",   "[60.2717,5.4950,40000],",  "[60.2358,5.6736,10175],",  "[60.2358,5.6642,10050],",
  "[60.2582,5.5383,9000],",   "[60.2329,5.6382,12150],",  "[60.2679,5.4724,5425],",   "[60.2521,5.5298,40000],",
  "[60.2358,5.5902,9200],",   "[60.2558,5.4865,9050],",   "[60.2355,5.5582,8775],",   "[60.2353,5.5480,8525],",
  "[60.2429,5.5138,11700],",  "[60.1789,5.7071,13625],",  "[60.1666,5.7231,13975],",  "[60.2586,5.4439,6550],",
  "[60.0830,5.9458,17225],",  "[60.2044,5.5829,41000],",  "[60.1312,5.7606,12275],",  "[60.2151,5.5353,41000],",
  "[60.1777,5.6154,37000],",  "[60.2242,5.4945,41000],",  "[60.1546,5.6447,10525],",  "[60.1271,5.6957,13250],",
  "[60.2308,5.4648,41000],",  "[60.1658,5.5900,9675],",   "[60.1501,5.6101,37000],",  "[60.1191,5.6691,36000],",
  "[60.1396,5.6080,37000],",  "[60.1074,5.6659,36000],",  "[60.1013,5.6642,36000],",  "[60.0918,5.6617,36000],",
  "[60.0877,5.6606,36000],",  "[60.0750,5.6572,36000],",  "[60.1096,5.6023,37000],",  "[60.0630,5.6540,36000],",
  "[60.1903,5.4678,7625],",   "[60.2502,5.3777,41000],",  "[60.2510,5.3738,41000],",  "[60.2329,5.3918,5975],",
  "[60.2836,5.3289,7300],",   "[60.1989,5.4242,6775],",   "[60.2343,5.3812,7125],",   "[60.2333,5.3770,7025],",
  "[60.2143,5.3840,9575],",   "[60.2576,5.3441,41000],",  "[60.2326,5.3646,5575],",   "[60.0226,5.5163,39000],",
  "[60.1791,5.3957,7625],",   "[59.7988,5.6481,39000],",  "[59.7975,5.6405,39000],",  "[59.8867,5.5668,36000],",
  "[59.8537,5.5541,37000],",  "[59.7749,5.5780,36000],",  "[59.7504,5.5716,36000],",  "[59.7260,5.5652,36000],",
  "[59.6944,5.5569,36000],",  "[59.9395,5.4430,36000],",  "[59.9436,5.4334,36000],",  "[60.1886,5.3404,6025],",
  "[60.1195,5.3585,39000],",  "[59.9625,5.3889,36000],",  "[59.6880,5.4429,17550],",  "[59.7589,5.4068,38975],",
  "[59.7388,5.3745,15050],",  "[59.7951,5.3642,13000],",  "[59.8869,5.3399,12375],",  "[59.9570,5.3216,14550],",
  "[59.9963,5.3088,36000],",  "[60.0033,5.2924,36000],",  "[59.6745,5.2716,36000],",  "[59.6756,5.2306,38000],",
  "[59.6791,5.2258,38000],",  "[59.6609,5.1876,35000],",  "[59.6673,5.1748,35000],",  "[59.9644,5.2198,12375],",
  "[59.7151,5.1460,39000],",  "[59.6779,5.1081,39000],",  "[59.7065,5.0950,39000],",  "[59.6958,5.0743,39000],",
  "[59.6988,5.0500,39000],",  "[59.4413,4.9136,37000],",  "[59.4333,4.8775,37000],",  "[59.1575,4.7032,36975],",
  "[59.1388,4.6444,35025],",  "[59.1858,4.6418,38975],",  "[59.1640,4.5850,35025],",  "[59.1853,4.5351,35025],",
  "[59.0284,4.4165,37000],",  "[58.9924,4.3522,38000],",  "[59.0112,4.3152,38000],",  "[59.0323,4.2734,38000],",
  "[58.7418,3.9515,35000],",  "[58.7558,3.9320,36875],",  "[58.6231,3.7231,40000],",  "[58.3588,3.4540,41000],",
  "[58.3970,3.4086,41000],",  "[58.2660,3.1820,36000],",  "[58.2854,3.1489,36000],",  "[58.3163,3.0959,36000],",
  "[58.3479,3.0417,36000],",  "[58.3710,2.8939,32000],",  "[58.3841,2.8190,37975],",  "[58.3123,2.6376,38000],",
  "[58.2307,2.4365,38000],",  "[58.0708,2.1231,41000],",  "[58.0813,2.1190,41000],",  "[58.0366,1.8705,36000],",
  "[58.1605,1.9359,35000],",  "[58.2207,1.9390,40000],",  "[58.1892,1.7784,40000],",  "[58.3750,2.0043,41000],",
  "[58.1323,1.4908,40000],",  "[58.4835,1.9613,41000],",  "[58.5353,1.9407,41000],",  "[58.5870,1.9202,41000],",
  "[58.6374,1.9000,41000],",  "[58.6700,1.8188,37000],",  "[58.7330,1.8617,41000],",  "[58.7795,1.8429,41000],",
  "[58.8257,1.8240,41000],",  "[58.8700,1.8060,41000],",  "[58.9137,1.7880,41000],",  "[58.9576,1.7704,41000],",
  "[59.0643,1.8668,39000],",  "[59.2049,2.1502,35975],",  "[59.2983,2.2999,41000],",  "[59.3229,2.2698,40000],",
  "[59.4797,2.6116,36000],",  "[59.4996,2.5795,36000],",  "[59.5596,2.5878,43000],",  "[59.5706,2.5692,43000],",
  "[59.7037,2.9045,37975],",  "[59.7072,2.8972,38000],",  "[59.5499,2.1125,38000],",  "[59.7512,2.8045,37975],",
  "[59.7742,2.7559,38000],",  "[59.7041,2.3423,43000],",  "[59.6878,2.2000,36000],",  "[59.7392,2.1886,36000],",
  "[59.7774,2.2165,43000],",  "[59.7721,2.1343,36000],",  "[59.7287,1.7453,41000],",  "[59.7910,1.7777,38000],",
  "[59.7976,1.7467,36000],",  "[59.8395,1.6006,40000],",  "[59.8602,1.5733,36000],",  "[59.8986,1.5214,41000],",
  "[59.9029,1.2635,41000],",  "[59.9349,1.2455,41000],",  "[59.9570,1.2330,41000],",  "[59.9787,1.2208,41000],",
  "[60.0373,1.1876,41000],",  "[60.0691,1.1695,41000],",  "[60.1107,1.1458,41000],",  "[60.1424,1.1277,41000],",
  "[60.1766,1.1081,41000],",  "[60.2191,1.0838,41000],",  "[60.2537,1.0454,40000],",  "[60.2903,0.9955,40000],",
  "[60.3269,0.9455,40000],",  "[60.3560,0.9057,40000],",  "[60.3709,0.8852,36000],",  "[60.4243,0.8118,36000],",
  "[60.4762,0.9349,41000],",  "[60.5162,0.9051,43000],",  "[60.5583,0.8280,43000],",  "[60.5997,0.7520,43000],",
  "[60.6287,0.6982,43000],",  "[60.6889,0.5864,43000],",  "[60.7148,0.7946,41000],",  "[60.7436,0.7570,40000],",
  "[60.8108,0.5487,36000],",  "[60.8960,0.1980,43000],",  "[60.9518,0.0921,43000],",  "[60.9958,0.0162,40000],",
  "[61.0368,-0.0729,43000],", "[61.1432,-0.2894,43000],", "[61.1968,-0.3986,43000],", "[61.1888,0.0746,38000],",
  "[61.2647,-0.1065,38000],", "[61.2273,0.4859,41000],",  "[61.2784,0.4545,41000],",  "[61.4677,-0.5451,38000],",
  "[61.4274,0.1919,45000],",  "[61.4687,0.0827,45000],",  "[61.4983,0.3183,41000],",  "[61.5652,0.2765,41000],",
  "[61.6289,0.2365,41000],",  "[61.6898,0.1981,41000],",  "[61.7582,0.1548,41000],",  "[61.8012,0.1273,41000],",
  "[61.8798,0.0765,41000],",  "[61.8278,0.5346,44700],",  "[62.0830,-0.2246,45000],", "[61.7203,1.2433,39000],",
  "[61.7950,1.1739,39000],",  "[61.8594,1.1137,39000],",  "[61.9479,1.0305,38975],",  "[61.9739,1.0059,39000],",
  "[61.8801,1.4463,38000],",  "[62.0348,1.2519,40450],",  "[62.0362,1.3132,40475],",  "[62.0387,1.4245,40625],",
  "[62.0943,1.5719,38000],",  "[62.2300,1.3441,38000],",  "[62.1026,1.8218,39000],",  "[62.5124,1.0914,40000],",
  "[62.6537,0.8357,41000],",  "[62.2135,1.8536,41000],",  "[62.0549,2.2798,41000],",  "[62.0565,2.3872,41000],",
  "[62.0581,2.4905,41000],",  "[62.8163,1.4203,37000],",  "[62.8654,1.3746,34000],",  "[62.8475,1.5461,33975],",
  "[62.8260,1.7500,34000],",  "[62.8111,1.8894,34000],",  "[62.7776,2.1974,38000],",  "[62.7762,2.2107,38000],",
  "[62.7577,2.3777,33975],",  "[62.7709,2.5688,40000],",  "[62.7190,2.6628,40000],",  "[62.0687,3.4972,41000],",
  "[62.2782,3.4423,40000],",  "[62.2692,3.4578,40000],",  "[62.2336,3.5858,34000],",  "[62.5231,3.4603,37000],",
  "[62.5162,3.5058,37000],",  "[62.4997,3.6140,37000],",  "[62.4735,3.7848,35000],",  "[62.6004,3.7264,38000],",
  "[62.4514,3.9268,35000],",  "[62.0760,4.2544,33975],",  "[62.0722,4.3287,41000],",  "[62.0722,4.4118,41000],",
  "[62.0722,4.4676,41000],",  "[62.3766,4.4401,38975],",  "[62.3575,4.5159,37000],",  "[62.3491,4.5678,37000],",
  "[62.3350,4.6541,37000],",  "[61.9308,4.8512,33950],",  "[61.7749,4.9513,34000],",  "[61.7582,5.0116,34000],",
  "[61.4997,5.1322,38000],",  "[61.7200,5.1497,34000],",  "[61.3872,5.2363,24000],",  "[61.1722,5.2811,36000]",
  "]}}}",
  NULL
};

/**
 * Code for `Modes.tmp_dir\\outline-test\\outline-test.bat`
 */
static const char *test_bat_code[] = {
  "@echo off",
  "setlocal",
  "cd /D %~dp0",
  "echo Starting 'py.exe -3 -m http.server'",
  "start py.exe -3 -m http.server",
  "echo Sleeping for 3 seconds...",
  "ping.exe -4 -n 3 localhost > NUL",
  "echo Starting 'chrome http://localhost:8000/show-outline.html'",
 /**
  * \todo Make this configurable
  */
  "\"c:\\Program Files\\Google\\Chrome\\Application\\chrome.exe\" \"http://localhost:8000/show-outline.html\"",
  NULL
};

/*
 * Create file under `dir == Modes.tmp_dir + \\outline-test\\`.
 * Lines in `*data`.
 */
static char *create_outline_file (const char *dir, const char *file, const char **data)
{
  char *fname = mg_mprintf ("%s\\%s", dir, file);
  FILE *f = fopen (fname, "w+");
  int   i;

  if (!f)
  {
    printf ("Failed to create %s, errno: %d\n", fname, errno);
    free (fname);
    return (NULL);
  }

  for (i = 0; data[i]; i++)
      fprintf (f, "%s\n", data[i]);

  printf ("Created file: '%s' with %d lines\n", fname, i);
  fclose (f);
  return (fname);
}

/**
 * Create a `show-outline.html` file and some outline-test JSON files
 * under `Modes.tmp_dir + /outline-test".
 *
 * Then use `outline-test.bat` to spawn a Python HTTP-server at port 8000
 * and spawn Chrome browser to show the result.
 *
 * \todo The latter should be configurable.
 */
static void aircraft_test_4 (unsigned max_num)
{
  char        dir [MAX_PATH];
  uint64_t    now = MSEC_TIME();
  size_t      size;
  char       *our_outline;
  char       *readsb_outline;
  char       *outline_html;
  char       *test_bat;
  char       *our_json;
  const char *our_data [2] = { NULL, NULL };

  printf ("%s(): Testing 'outline' stuff:\n", __FUNCTION__);

  g_data.outline_enable = true;

  snprintf (dir, sizeof(dir), "%s\\outline-test", Modes.tmp_dir);
  if (!CreateDirectory(dir, 0) && GetLastError() != ERROR_ALREADY_EXISTS)
     printf ("'CreateDirectory(\"%s\")' failed; %s.\n", dir, win_strerror(GetLastError()));

  /* Use data from `aircraft_test_3()`
   */
  for (unsigned i = 0; i < max_num; i++)
      aircraft_outline_update (aircraft_find(0x470000 + i), now);

  our_outline = mg_mprintf ("%s\\%s", dir, "our_outline.json");
  aircraft_outline_generate (now, our_outline);

  our_json     = aircraft_outline_json (our_outline, &size);
  our_data [0] = our_json;
  our_json [size-2] = '\0';   /* remove last `\n` */

  /* This effectively copies data in `g_data.outline_json` to
   * `Modes.tmp_dir\\outline-test\\our_outline_json`.
   */
  free (our_outline);
  our_outline    = create_outline_file (dir, "our_outline.json", our_data);
  readsb_outline = create_outline_file (dir, "readsb_outline.json", readsb_outline_json);
  outline_html   = create_outline_file (dir, "show-outline.html", show_outline_html);
  test_bat       = create_outline_file (dir, "outline-test.bat", test_bat_code);

  if (test_bat)
  {
    int rc = system (test_bat);
    if (rc == 0)
       printf ("%s OK.\n\n", test_bat);
    else if (rc != 0)
       printf ("%s failed; errno: %d/%s\n", test_bat, errno, strerror(errno));
  }
  free (our_outline);
  free (our_json);
  free (outline_html);
  free (test_bat);
  free (readsb_outline);
  puts ("");
}

static bool aircraft_tests (void)
{
  unsigned max_num = g_data.csv_len;

#if defined(USE_BIN_FILES)
  if (aircraft_init_BIN() && max_num > Modes.bin.aircrafts_records_num)
     max_num = Modes.bin.aircrafts_records_num;    /* make it overlapping 0 - max_num */
#endif

  aircraft_test_1();
  aircraft_test_2 (max_num);
  aircraft_test_3 (50);
  aircraft_test_4 (50);
  return (true);
}

/**
 * The callback called from `zip_extract()`.
 */
static int extract_callback (const char *file, void *arg)
{
  const char *tmp_file = arg;

  TRACE ("Copying extracted file '%s' to '%s'\n", file, tmp_file);
  if (!CopyFileA (file, tmp_file, FALSE))
     LOG_STDERR ("CopyFileA (\"%s\", \"%s\") failed: %s\n", file, tmp_file, win_strerror(GetLastError()));
  return (0);
}

/**
 * Check if the aircraft .CSV-database is older than `CSV_MAX_AGE` days.
 * If so:
 *  1) download the OpenSky .zip file to `%TEMP%\\dump1090\\aircraft-database-temp.zip`,
 *  2) calls `zip_extract (\"%TEMP%\\dump1090\\aircraft-database-temp.zip\", \"%TEMP%\\dump1090\\aircraft-database-temp.csv\")`.
 *  3) copy `%TEMP%\\dump1090\\aircraft-database-temp.csv` over to 'db_file'.
 *  4) remove `g_data.sql_file` to force a rebuild of it.
 */
bool aircraft_update_CSV (const char *db_file, const char *url, time_t max_age)
{
  struct stat st;
  bool   force_it = false;
  char   tmp_file [MAX_PATH];
  char   zip_file [MAX_PATH];

  if (!db_file || !db_file[0] || !url)
  {
    LOG_STDERR ("Illegal parameters; db_file='%s', url='%s'.\n", db_file, url);
    return (false);
  }

  memset (&st, '\0', sizeof(st));
  if (stat(db_file, &st) != 0)
  {
    LOG_STDERR ("\nForce updating '%s' since it does not exist.\n", db_file);
    force_it = true;
  }

  snprintf (zip_file, sizeof(zip_file), "%s\\%s.zip", Modes.tmp_dir, AIRCRAFT_DATABASE_TMP);
  if (stat(zip_file, &st) || st.st_size == 0)
  {
    LOG_STDERR ("\nFile '%s' doesn't exist (or is truncated). Forcing a download.\n",
                zip_file);
    force_it = true;
  }

  if (!force_it)
  {
    time_t when, now = time (NULL);
    time_t expiry = now - max_age;

    if (st.st_mtime > expiry)
    {
      when = now + max_age;  /* max_age into the future */

      if (Modes.update)
         LOG_STDERR ("\nUpdate of '%s' not needed before %.24s.\n", zip_file, ctime(&when));
      return (true);
    }
  }

  LOG_STDERR ("%supdating '%s' from '%s'\n", force_it ? "Force " : "", zip_file, url);

  if (download_to_file(url, zip_file) <= 0)
  {
    LOG_STDERR ("Failed to download '%s': '%s'\n", zip_file, Modes.wininet_last_error);
    return (false);
  }

  snprintf (tmp_file, sizeof(tmp_file), "%s\\%s.csv", Modes.tmp_dir, AIRCRAFT_DATABASE_TMP);

  int rc = zip_extract (zip_file, Modes.tmp_dir, extract_callback, tmp_file);
  if (rc < 0)
  {
    LOG_STDERR ("Failed in call to 'zip_extract()': %s (%d)\n", zip_strerror(rc), rc);
    return (false);
  }

  if (!CopyFileA(tmp_file, db_file, FALSE))
  {
    LOG_STDERR ("CopyFileA (\"%s\", \"%s\") failed: %s\n", tmp_file, db_file, win_strerror(GetLastError()));
    return (false);
  }
  else
  {
    LOG_STDERR ("Copied '%s' -> '%s'\n", tmp_file, db_file);
    touch_file (db_file);
  }

  LOG_STDERR ("Deleting '%s' to force a rebuild in 'aircraft_load_CSV()'\n", g_data.sql_file);
  DeleteFileA (g_data.sql_file);
  g_data.sql_file_found = false;
  return (true);
}

/**
 * The CSV callback for adding records to `g_data.csv_list`.
 *
 * \param[in]  ctx   the CSV context structure.
 * \param[in]  value the value for this CSV field in record `ctx->rec_num`.
 *
 * Match the fields 0, 1, 3, 8 and 10 for a record like this:
 * ```
 * "icao24","registration","manufacturericao","manufacturername","model","typecode","serialnumber","linenumber",
 * "icaoaircrafttype","operator","operatorcallsign","operatoricao","operatoriata","owner","testreg","registered",
 * "reguntil","status","built","firstflightdate","seatconfiguration","engines","modes","adsb","acars","notes",
 * "categoryDescription"
 * ```
 *
 * 27 fields!
 */
static int CSV_callback (struct CSV_context *ctx, const char *value)
{
  int rc = 1;

  if (ctx->field_num == 0)        /* "icao24" field */
  {
    g_data.current_rec.addr = mg_unhex (value);
  }
  else if (ctx->field_num == 1)   /* "registration" field */
  {
    strncpy (g_data.current_rec.reg_num, value, sizeof(g_data.current_rec.reg_num)-1);
  }
  else if (ctx->field_num == 3)   /* "manufacturername" field */
  {
    strncpy (g_data.current_rec.manufact, value, sizeof(g_data.current_rec.manufact)-1);
  }
  else if (ctx->field_num == 8)  /* "icaoaircrafttype" field */
  {
    strncpy (g_data.current_rec.type, value, sizeof(g_data.current_rec.type)-1);
  }
  else if (ctx->field_num == 10)  /* "operatorcallsign" field */
  {
    strncpy (g_data.current_rec.call_sign, value, sizeof(g_data.current_rec.call_sign)-1);
  }
  else if (ctx->field_num == ctx->num_fields - 1)  /* we got the last field */
  {
    rc = CSV_add_entry (&g_data.current_rec);
    memset (&g_data.current_rec, '\0', sizeof(g_data.current_rec));  /* ready for another record */
  }
  return (rc);
}

/**
 * Initialize the SQL aircraft-database name.
 */
static void aircraft_SQL_set_name (void)
{
  if (strcmp(g_data.csv_file, "NUL"))
  {
    struct stat st;

    memset (&st, '\0', sizeof(st));
    g_data.sql_file = mg_mprintf ("%s.sqlite", g_data.csv_file);
    g_data.sql_file_found = (stat (g_data.sql_file, &st) == 0) && (st.st_size > 8*1024);

    TRACE ("Aircraft Sqlite database \"%s\", size: %ld\n",
           g_data.sql_file, g_data.sql_file_found ? st.st_size : 0);

    if (!g_data.sql_file_found)
       DeleteFileA (g_data.sql_file);   /* delete it in case it's truncated */
  }
  else
  {
    g_data.sql_file_found = false;
    free (g_data.sql_file);
    g_data.sql_file = NULL;
  }
}

/**
 * Initialize the SQLite interface and aircraft-database from the .csv.sqlite file.
 */
static bool aircraft_SQL_load (bool *created, bool *opened, struct stat *st_csv, double *load_t)
{
  struct stat st;
  int    diff_days;
  double usec;

  *created = *opened = false;
  *load_t = 0.0;

  if (sqlite3_initialize() != SQLITE_OK)
  {
    LOG_STDERR ("Sqlite init failed.\n" );
    return (false);
  }

  if (!g_data.sql_file_found)
  {
    TRACE ("Aircraft Sqlite database \"%s\" does not exist.\n"
           "Creating new from \"%s\".\n\n", g_data.sql_file, g_data.csv_file);
    *created = sql_create();
    if (*created)
       g_data.sql_file_found = true;
    return (false);
  }

  memset (&st, '\0', sizeof(st));
  stat (g_data.sql_file, &st);
  diff_days = (st_csv->st_mtime - st.st_mtime) / (3600*24);

  TRACE ("'%s' is %d days %s than the CSV-file\n",
         g_data.sql_file, abs(diff_days), diff_days < 0 ? "newer" : "older");

  usec = get_usec_now();
  *opened = sql_open();
  *load_t = get_usec_now() - usec;
  return (true);
}

/**
 * Initialize the aircraft-database from .csv file.
 *
 * But if the .sqlite file exist, use that instead.
 */
static bool aircraft_load_CSV (void)
{
  struct stat st_csv;
  double usec;
  double csv_load_t   = 0.0;
  double sql_load_t   = 0.0;
  double sql_create_t = 0.0;
  bool   sql_created  = false;
  bool   sql_opened   = false;

  assert (!g_data.csv_done);   /* Do this only once */
  g_data.csv_done = true;

  if (!stricmp(g_data.csv_file, "NUL"))   /* User want no '.csv' or '.csv.sqlite' file */
     return (true);

  if (stat(g_data.csv_file, &st_csv) != 0)
  {
    LOG_STDERR ("Aircraft database \"%s\" does not exist.\n"
                "Do a \"%s --update\" to download and generate it.\n",
                g_data.csv_file, Modes.who_am_I);
    return (false);
  }

  aircraft_SQL_load (&sql_created, &sql_opened, &st_csv, &sql_load_t);

  if (g_data.sql_file)
     LOG_STDERR ("%susing Sqlite file: \"%s\".\n", g_data.sql_file_found ? "" : "Not ", g_data.sql_file);

  /**
   * In `g_data.test_mode`, open and parse the .CSV-file to compare speed
   * of `g_data.csv_list` lookup vs. `SQL_lookup_entry()` lookup.
   */
  if (!sql_opened || sql_created || g_data.test_mode)
  {
    memset (&g_data.csv_ctx, '\0', sizeof(g_data.csv_ctx));
    g_data.csv_ctx.file_name = g_data.csv_file;
    g_data.csv_ctx.delimiter = ',';
    g_data.csv_ctx.callback  = CSV_callback;
    g_data.csv_ctx.line_size = 2000;

    usec = get_usec_now();
    LOG_STDOUT ("Loading '%s' will take some time.\n", g_data.csv_file);

    if (!CSV_open_and_parse_file(&g_data.csv_ctx))
    {
      LOG_STDERR ("Parsing of \"%s\" failed: %s\n", g_data.csv_file, strerror(errno));
      return (false);
    }

    TRACE ("Parsed %u records from: \"%s\"\n", g_data.csv_len, g_data.csv_file);

    if (g_data.csv_len > 0)
    {
      qsort (g_data.csv_list, g_data.csv_len, sizeof(*g_data.csv_list),
             CSV_compare_on_addr);
      csv_load_t = get_usec_now() - usec;
    }
  }

  if (sql_created && g_data.csv_len > 0)
  {
    const aircraft_info *ai = g_data.csv_list + 0;
    uint32_t i;

    LOG_STDOUT ("Creating SQL-database '%s'... ", g_data.sql_file);
    usec = get_usec_now();
    sql_begin();

    for (i = 0; i < g_data.csv_len; i++, ai++)
        if (!sql_add_entry (i, ai))
           break;

    sql_end();
    sql_create_t = get_usec_now() - usec;

    if (i != g_data.csv_len)
         LOG_STDOUT ("\nCreated only %u out of %u records!\n", i, g_data.csv_len);
    else LOG_STDOUT ("\nCreated %u records\n", g_data.csv_len);
  }

  if (g_data.test_mode)
  {
    TRACE ("CSV loaded and parsed in %.3f ms\n", csv_load_t / 1E3);
    if (sql_create_t > 0.0)
         TRACE ("SQL created in %.3f ms\n", sql_create_t / 1E3);
    else TRACE ("SQL loaded in %.3f ms\n", sql_load_t / 1E3);

    return aircraft_tests();
  }
  return (true);
}

/*
 * Declare ICAO registration address ranges and country.
 * Mostly generated from the assignment table in the appendix to Chapter 9 of
 * Annex 10 Vol III, Second Edition, July 2007 (with amendments through 88-A, 14/11/2013)
 *
 * Rewritten from `web_root-Tar1090/flags.js` to lookup
 * the county.
 *
 * The low and high values used to lookup a (short/long) country
 * or military ranges.
 */
typedef struct ICAO_range {
        uint32_t    low;
        uint32_t    high;
        const char *cc_short;
        const char *cc_long;
      } ICAO_range;

static const ICAO_range ICAO_ranges [] = {
    { 0x004000, 0x0047FF, "ZW", "Zimbabwe" },
    { 0x006000, 0x006FFF, "MZ", "Mozambique" },
    { 0x008000, 0x00FFFF, "ZA", "South Africa" },
    { 0x010000, 0x017FFF, "EG", "Egypt" },
    { 0x018000, 0x01FFFF, "LY", "Libya"  },
    { 0x020000, 0x027FFF, "MA", "Morocco" },
    { 0x028000, 0x02FFFF, "TN", "Tunisia" },
    { 0x030000, 0x0307FF, "BW", "Botswana" },
    { 0x032000, 0x032FFF, "BI", "Burundi" },
    { 0x034000, 0x034FFF, "CM", "Cameroon" },
    { 0x035000, 0x0353FF, "KM", "Comoros" },
    { 0x036000, 0x036FFF, "CG", "Congo" },
    { 0x038000, 0x038FFF, "CI", "Cote d'Ivoire" },
    { 0x03E000, 0x03EFFF, "GA", "Gabon" },
    { 0x040000, 0x040FFF, "ET", "Ethiopia" },
    { 0x042000, 0x042FFF, "GQ", "Equatorial Guinea" },
    { 0x044000, 0x044FFF, "GH", "Ghana" },
    { 0x046000, 0x046FFF, "GN", "Guinea" },
    { 0x048000, 0x0487FF, "GW", "Guinea-Bissau" },
    { 0x04A000, 0x04A7FF, "LS", "Lesotho" },
    { 0x04C000, 0x04CFFF, "KE", "Kenya" },
    { 0x050000, 0x050FFF, "LR", "Liberia" },
    { 0x054000, 0x054FFF, "MG", "Madagascar" },
    { 0x058000, 0x058FFF, "MW", "Malawi" },
    { 0x05A000, 0x05A7FF, "MV", "Maldives" },
    { 0x05C000, 0x05CFFF, "ML", "Mali" },
    { 0x05E000, 0x05E7FF, "MR", "Mauritania" },
    { 0x060000, 0x0607FF, "MU", "Mauritius" },
    { 0x062000, 0x062FFF, "NE", "Niger" },
    { 0x064000, 0x064FFF, "NG", "Nigeria" },
    { 0x068000, 0x068FFF, "UG", "Uganda" },
    { 0x06A000, 0x06AFFF, "QA", "Qatar" },
    { 0x06C000, 0x06CFFF, "CF", "Central African Republic" },
    { 0x06E000, 0x06EFFF, "RW", "Rwanda" },
    { 0x070000, 0x070FFF, "SN", "Senegal" },
    { 0x074000, 0x0747FF, "SC", "Seychelles" },
    { 0x076000, 0x0767FF, "SL", "Sierra Leone" },
    { 0x078000, 0x078FFF, "SO", "Somalia" },
    { 0x07A000, 0x07A7FF, "SZ", "Eswatini" },
    { 0x07C000, 0x07C3FF, "SD", "Sudan" },
    { 0x080000, 0x080FFF, "TZ", "Tanzania" },
    { 0x084000, 0x084FFF, "TD", "Chad" },
    { 0x088000, 0x088FFF, "TG", "Togo" },
    { 0x08A000, 0x08AFFF, "ZM", "Zambia" },
    { 0x08C000, 0x08CFFF, "CD", "DR Congo" },
    { 0x090000, 0x090FFF, "AO", "Angola" },
    { 0x094000, 0x0947FF, "BJ", "Benin" },
    { 0x096000, 0x0967FF, "CV", "Cape Verde" },
    { 0x098000, 0x0987FF, "DJ", "Djibouti" },
    { 0x09A000, 0x09AFFF, "GM", "Gambia" },
    { 0x09C000, 0x09CFFF, "BF", "Burkina Faso" },
    { 0x0A0000, 0x0A7FFF, "DZ", "Algeria" },
    { 0x0A8000, 0x0A8FFF, "BS", "Bahamas" },
    { 0x0AA000, 0x0AA3FF, "BB", "Barbados" },
    { 0x0AB000, 0x0AB7FF, "BZ", "Belize" },
    { 0x0AC000, 0x0ACFFF, "CO", "Colombia" },
    { 0x0AE000, 0x0AEFFF, "CR", "Costa Rica" },
    { 0x0B0000, 0x0B0FFF, "CU", "Cuba" },
    { 0x0B2000, 0x0B2FFF, "SV", "El Salvador" },
    { 0x0B4000, 0x0B4FFF, "GT", "Guatemala" },
    { 0x0B6000, 0x0B6FFF, "GY", "Guyana" },
    { 0x0B8000, 0x0B8FFF, "HT", "Haiti" },
    { 0x0BA000, 0x0BAFFF, "HN", "Honduras" },
    { 0x0BC000, 0x0BC7FF, "VC", "Saint Vincent & the Grenadines" },
    { 0x0BE000, 0x0BEFFF, "JM", "Jamaica" },
    { 0x0C0000, 0x0C0FFF, "NI", "Nicaragua" },
    { 0x0C2000, 0x0C2FFF, "PA", "Panama" },
    { 0x0C4000, 0x0C4FFF, "DO", "Dominican Republic" },
    { 0x0C6000, 0x0C6FFF, "TT", "Trinidad & Tobago" },
    { 0x0C8000, 0x0C8FFF, "SR", "Suriname" },
    { 0x0CA000, 0x0CA3FF, "AG", "Antigua & Barbuda" },
    { 0x0CC000, 0x0CC7FF, "GD", "Grenada" },
    { 0x0D0000, 0x0D7FFF, "MX", "Mexico" },
    { 0x0D8000, 0x0DFFFF, "VE", "Venezuela" },
    { 0x100000, 0x1FFFFF, "RU", "Russia" },
    { 0x201000, 0x2017FF, "NA", "Namibia" },
    { 0x202000, 0x2027FF, "ER", "Eritrea" },
    { 0x300000, 0x33FFFF, "IT", "Italy" },
    { 0x340000, 0x37FFFF, "ES", "Spain" },
    { 0x380000, 0x3BFFFF, "FR", "France" },
    { 0x3C0000, 0x3FFFFF, "DE", "Germany" },

    /* UK territories are officially part of the UK range
     * add extra entries that are above the UK and take precedence
     * this is a mess ... let's still try
     */
    { 0x400000, 0x4001BF, "BM", "Bermuda" },
    { 0x4001C0, 0x4001FF, "KY", "Cayman Islands" },
    { 0x400300, 0x4003FF, "TC", "Turks & Caicos Islands" },
    { 0x424135, 0x4241F2, "KY", "Cayman Islands" },
    { 0x424200, 0x4246FF, "BM", "Bermuda" },
    { 0x424700, 0x424899, "KY", "Cayman Islands" },
    { 0x424B00, 0x424BFF, "IM", "Isle of Man" },
    { 0x43BE00, 0x43BEFF, "BM", "Bermuda" },
    { 0x43E700, 0x43EAFD, "IM", "Isle of Man" },
    { 0x43EAFE, 0x43EEFF, "GG", "Guernsey" },

    /* Catch all United Kingdom for the even more obscure stuff
     */
    { 0x400000, 0x43FFFF, "GB", "United Kingdom" },
    { 0x440000, 0x447FFF, "AT", "Austria" },
    { 0x448000, 0x44FFFF, "BE", "Belgium" },
    { 0x450000, 0x457FFF, "BG", "Bulgaria" },
    { 0x458000, 0x45FFFF, "DK", "Denmark" },
    { 0x460000, 0x467FFF, "FI", "Finland" },
    { 0x468000, 0x46FFFF, "GR", "Greece" },
    { 0x470000, 0x477FFF, "HU", "Hungary" },
    { 0x478000, 0x47FFFF, "NO", "Norway" },
    { 0x480000, 0x487FFF, "NL", "Netherland" },
    { 0x488000, 0x48FFFF, "PL", "Poland" },
    { 0x490000, 0x497FFF, "PT", "Portugal" },
    { 0x498000, 0x49FFFF, "CZ", "Czechia" },  /* previously 'Czech Republic' */
    { 0x4A0000, 0x4A7FFF, "RO", "Romania" },
    { 0x4A8000, 0x4AFFFF, "SE", "Sweden" },
    { 0x4B0000, 0x4B7FFF, "CH", "Switzerland" },
    { 0x4B8000, 0x4BFFFF, "TR", "Turkey" },
    { 0x4C0000, 0x4C7FFF, "RS", "Serbia" },
    { 0x4C8000, 0x4C87FF, "CY", "Cyprus" },
    { 0x4CA000, 0x4CAFFF, "IE", "Ireland" },
    { 0x4CC000, 0x4CCFFF, "IS", "Iceland" },
    { 0x4D0000, 0x4D03FF, "LU", "Luxembourg" },
    { 0x4D2000, 0x4D2FFF, "MT", "Malta" },
    { 0x4D4000, 0x4D43FF, "MC", "Monaco" },
    { 0x500000, 0x5003FF, "SM", "San Marino" },
    { 0x501000, 0x5013FF, "AL", "Albania" },
    { 0x501800, 0x501FFF, "HR", "Croatia" },
    { 0x502800, 0x502FFF, "LV", "Latvia" },
    { 0x503800, 0x503FFF, "LT", "Lithuania" },
    { 0x504800, 0x504FFF, "MD", "Moldova"  },
    { 0x505C00, 0x505FFF, "SK", "Slovakia" },
    { 0x506000, 0x506FFF, "SI", "Slovenia" },
    { 0x507000, 0x507FFF, "UZ", "Uzbekistan" },
    { 0x508000, 0x50FFFF, "UA", "Ukraine" },
    { 0x510000, 0x5107FF, "BY", "Belarus" },
    { 0x511000, 0x5117FF, "EE", "Estonia" },
    { 0x512000, 0x5127FF, "MK", "Macedonia" },
    { 0x513000, 0x5137FF, "BA", "Bosnia & Herzegovina" },
    { 0x514000, 0x5147FF, "GE", "Georgia" },
    { 0x515000, 0x5157FF, "TJ", "Tajikistan" },
    { 0x516000, 0x5167FF, "ME", "Montenegro" },
    { 0x600000, 0x6007FF, "AM", "Armenia" },
    { 0x600800, 0x600FFF, "AZ", "Azerbaijan" },
    { 0x601000, 0x6017FF, "KG", "Kyrgyzstan" },
    { 0x601800, 0x601FFF, "TM", "Turkmenistan" },
    { 0x680000, 0x6807FF, "BT", "Bhutan" },
    { 0x681000, 0x6817FF, "FM", "Micronesia" },
    { 0x682000, 0x6827FF, "MN", "Mongolia" },
    { 0x683000, 0x6837FF, "KZ", "Kazakhstan" },
    { 0x684000, 0x6847FF, "PW", "Palau" },
    { 0x700000, 0x700FFF, "AF", "Afghanistan" },
    { 0x702000, 0x702FFF, "BD", "Bangladesh" },
    { 0x704000, 0x704FFF, "MM", "Myanmar" },
    { 0x706000, 0x706FFF, "KW", "Kuwait" },
    { 0x708000, 0x708FFF, "LA", "Laos" },
    { 0x70A000, 0x70AFFF, "NP", "Nepal" },
    { 0x70C000, 0x70C7FF, "OM", "Oman" },
    { 0x70E000, 0x70EFFF, "KH", "Cambodia"},
    { 0x710000, 0x717FFF, "SA", "Saudi Arabia" },
    { 0x718000, 0x71FFFF, "KR", "South Korea" },
    { 0x720000, 0x727FFF, "KP", "North Korea" },
    { 0x728000, 0x72FFFF, "IQ", "Iraq" },
    { 0x730000, 0x737FFF, "IR", "Iran" },
    { 0x738000, 0x73FFFF, "IL", "Israel" },
    { 0x740000, 0x747FFF, "JO", "Jordan" },
    { 0x748000, 0x74FFFF, "LB", "Lebanon" },
    { 0x750000, 0x757FFF, "MY", "Malaysia" },
    { 0x758000, 0x75FFFF, "PH", "Philippines" },
    { 0x760000, 0x767FFF, "PK", "Pakistan" },
    { 0x768000, 0x76FFFF, "SG", "Singapore" },
    { 0x770000, 0x777FFF, "LLK", "Sri Lanka" },
    { 0x778000, 0x77FFFF, "SY", "Syria" },
    { 0x789000, 0x789FFF, "HK", "Hong Kong" },
    { 0x780000, 0x7BFFFF, "CN", "China" },
    { 0x7C0000, 0x7FFFFF, "AU", "Australia" },
    { 0x800000, 0x83FFFF, "IN", "India" },
    { 0x840000, 0x87FFFF, "JP", "Japan" },
    { 0x880000, 0x887FFF, "TH", "Thailand" },
    { 0x888000, 0x88FFFF, "VN", "Viet Nam" },
    { 0x890000, 0x890FFF, "YE", "Yemen" },
    { 0x894000, 0x894FFF, "BH", "Bahrain" },
    { 0x895000, 0x8957FF, "BN", "Brunei" },
    { 0x896000, 0x896FFF, "AE", "United Arab Emirates" },
    { 0x897000, 0x8977FF, "SB", "Solomon Islands" },
    { 0x898000, 0x898FFF, "PG", "Papua New Guinea" },
    { 0x899000, 0x8997FF, "TW", "Taiwan" },
    { 0x8A0000, 0x8A7FFF, "ID", "Indonesia"  },
    { 0x900000, 0x9007FF, "MH", "Marshall Islands" },
    { 0x901000, 0x9017FF, "CK", "Cook Islands" },
    { 0x902000, 0x9027FF, "WS", "Samoa" },
    { 0x9E0000, 0x9E07FF, "ST", "Sao Tome & Principe" },
    { 0xA00000, 0xAFFFFF, "US", "United States" },
    { 0xC00000, 0xC3FFFF, "CA", "Canada" },
    { 0xC80000, 0xC87FFF, "NZ", "New Zealand" },
    { 0xC88000, 0xC88FFF, "FJ", "Fiji" },
    { 0xC8A000, 0xC8A7FF, "NR", "Nauru" },
    { 0xC8C000, 0xC8C7FF, "LC", "Saint Lucia" },
    { 0xC8D000, 0xC8D3FF, "TU", "Tonga" },
    { 0xC8E000, 0xC8E7FF, "KI", "Kiribati" },
    { 0xC90000, 0xC907FF, "VU", "Vanuatu" },
    { 0xC91000, 0xC917FF, "AD", "Andorra" },
    { 0xC92000, 0xC927FF, "DM", "Dominica" },
    { 0xC93000, 0xC937FF, "KN", "Saint Kitts and Nevis" },
    { 0xC94000, 0xC947FF, "SS", "South Sudan" },
    { 0xC95000, 0xC957FF, "TL", "Timor-Leste" },
    { 0xC97000, 0xC977FF, "TV", "Tuvalu" },
    { 0xE00000, 0xE3FFFF, "AR", "Argentina" },
    { 0xE40000, 0xE7FFFF, "BR", "Brazil" },
    { 0xE80000, 0xE80FFF, "CL", "Chile" },
    { 0xE84000, 0xE84FFF, "EC", "Ecuador" },
    { 0xE88000, 0xE88FFF, "PY", "Paraguay" },
    { 0xE8C000, 0xE8CFFF, "PE", "Peru" },
    { 0xE90000, 0xE90FFF, "UY", "Uruguay" },
    { 0xE94000, 0xE94FFF, "BO", "Bolivia" },
    { 0xF00000, 0xF07FFF, "",   "ICAO (temporary)" },
    { 0xF09000, 0xF093FF, "",   "ICAO (special use)" },
    { 0xF09000, 0xF097FF, "",   "ICAO (special use)" }
};

const char *aircraft_get_country (uint32_t addr, bool get_short)
{
  const ICAO_range *r = ICAO_ranges + 0;
  uint16_t   i;

  for (i = 0; i < DIM(ICAO_ranges); i++, r++)
      if (addr >= r->low && addr <= r->high)
         return (get_short ? r->cc_short : r->cc_long);
  return (NULL);
}

/**
 * Returns TRUE if the ICAO address is in one of these military ranges.
 */
static const ICAO_range military_range [] = {
     { 0xADF7C8,  0xAFFFFF, "US" },
     { 0x010070,  0x01008F, NULL },
     { 0x0A4000,  0x0A4FFF, NULL },
     { 0x33FF00,  0x33FFFF, NULL },
     { 0x350000,  0x37FFFF, NULL },
     { 0x3A8000,  0x3AFFFF, NULL },
     { 0x3B0000,  0x3BFFFF, NULL },
     { 0x3EA000,  0x3EBFFF, NULL },
     { 0x3F4000,  0x3FBFFF, NULL },
     { 0x400000,  0x40003F, NULL },
     { 0x43C000,  0x43CFFF, "GB" },
     { 0x444000,  0x446FFF, NULL },
     { 0x44F000,  0x44FFFF, NULL },
     { 0x457000,  0x457FFF, NULL },
     { 0x45F400,  0x45F4FF, NULL },
     { 0x468000,  0x4683FF, NULL },
     { 0x473C00,  0x473C0F, NULL },
     { 0x478100,  0x4781FF, NULL },
     { 0x480000,  0x480FFF, NULL },
     { 0x48D800,  0x48D87F, NULL },
     { 0x497C00,  0x497CFF, NULL },
     { 0x498420,  0x49842F, NULL },
     { 0x4B7000,  0x4B7FFF, NULL },
     { 0x4B8200,  0x4B82FF, NULL },
     { 0x506F00,  0x506FFF, NULL },
     { 0x70C070,  0x70C07F, NULL },
     { 0x710258,  0x71028F, NULL },
     { 0x710380,  0x71039F, NULL },
     { 0x738A00,  0x738AFF, NULL },
     { 0x7C822E,  0x7C84FF, NULL },
     { 0x7C8800,  0x7C88FF, NULL },
     { 0x7C9000,  0x7CBFFF, NULL },
     { 0x7CF800,  0x7CFAFF, "AU" },
     { 0x7D0000,  0x7FFFFF, NULL },
     { 0x800200,  0x8002FF, NULL },
     { 0xC0CDF9,  0xC3FFFF, "CA" },
     { 0xC87F00,  0xC87FFF, "NZ" },
     { 0xE40000,  0xE41FFF, NULL }
   };

bool aircraft_is_military (uint32_t addr, const char **country)
{
  const ICAO_range *r = military_range + 0;
  uint16_t          i;

  for (i = 0; i < DIM(military_range); i++, r++)
      if (addr >= r->low && addr <= r->high)
      {
        if (country && r->cc_short)
           *country = r->cc_short;
        return (true);
      }
  return (false);
}

#if defined(USE_BIN_FILES)
/*
 * Generated by `py -3 ../tools/gen_data.py --gen-c $(OBJ_DIR)/gen-code-blocks.c'
 */
#include "gen-code-blocks.c"

bool aircraft_is_military2 (uint32_t addr, const char **country)
{
  const blocks_record *br = aircraft_find_block (addr);

  if (br && br->is_military)
  {
    if (country)
       *country = br->country_ISO;
    return (true);
  }
  return (false);
}

const char *aircraft_get_country2 (uint32_t addr, bool get_short)
{
  const blocks_record *br = aircraft_find_block (addr);
  const ICAO_range    *ir = ICAO_ranges + 0;
  uint16_t i;

  if (!br)
     return (NULL);

  if (get_short)
     return (br->country_ISO);

  /* Find the cc_long name matching cc_short in `b->country_ISO'
   */
  for (i = 0; i < DIM(ICAO_ranges); i++, ir++)
  {
    if (!stricmp(br->country_ISO, ir->cc_short))
       return (ir->cc_long);
  }
  return (NULL);
}
#endif  /* USE_BIN_FILES */

/**
 * The types of a helicopter (incomplete).
 */
static bool is_helicopter_type (const char *type)
{
  const char *heli_types[] = { "H1P", "H2P", "H1T", "H2T" };
  uint16_t    i;

  if (type[0] != 'H')   /* must start with a 'H' */
     return (false);

  for (i = 0; i < DIM(heli_types); i++)
      if (!stricmp(type, heli_types[i]))
         return (true);
  return (false);
}

/**
 * Figure out if an ICAO-address belongs to a helicopter.
 */
bool aircraft_is_helicopter (uint32_t addr, const char **type_ptr)
{
  const aircraft_info *ai;

  if (type_ptr)
     *type_ptr = NULL;

  ai = CSV_lookup_entry (addr);
  if (ai && is_helicopter_type(ai->type))
  {
    if (type_ptr)
       *type_ptr = ai->type;
    return (true);
  }
  return (false);
}

/**
 * Convert 24-bit big-endian (network order) to host order format.
 */
uint32_t aircraft_get_addr (const uint8_t *addr)
{
  return ((addr[0] << 16) | (addr[1] << 8) | addr[2]);
}

const char *aircraft_get_military (uint32_t addr)
{
  static char  buf [20];
  const  char *cntry;
  bool   mil = (*g_data.p_is_military) (addr, &cntry);
  int    sz;

  if (!mil)
     return ("");

  sz = snprintf (buf, sizeof(buf), "Military");
  if (cntry)
     snprintf (buf+sz, sizeof(buf)-sz, " (%s)", cntry);
  return (buf);
}

static const char *addrtype_to_string (addrtype_t type)
{
  static const search_list types[] = {
             { ADDR_ADSB_ICAO,      "Mode S / ADS-B" },
             { ADDR_ADSB_ICAO_NT,   "ADS-B, non-transponder" },
             { ADDR_ADSB_OTHER,     "ADS-B, other addressing scheme" },
             { ADDR_TISB_ICAO,      "TIS-B" },
             { ADDR_TISB_OTHER,     "TIS-B, other addressing scheme" },
             { ADDR_TISB_TRACKFILE, "TIS-B, Mode A code and track file number" },
             { ADDR_ADSR_ICAO,      "ADS-R" },
             { ADDR_ADSR_OTHER,     "ADS-R, other addressing scheme" },
             { ADDR_MODE_A,         "Mode A" }
             };
  return search_list_name (type, types, DIM(types));
}

/**
 * Return the hex-string for the 24-bit ICAO address in `addr`.
 * Also look for the registration number and manufacturer from
 * the CSV or SQL data structures.
 */
const char *aircraft_get_details (const modeS_message *mm)
{
  static char          buf [100];
  const aircraft_info *a;
  char                *p   = buf;
  char                *end = buf + sizeof(buf);
  uint32_t             addr;
  bool                 unused;

  p += snprintf (p, end - p, "%s", addrtype_to_string(mm->addrtype));

  addr = mm->addr;
  a = aircraft_lookup (addr, &unused, &unused);

  if (a && a->reg_num[0])
     snprintf (p, end - p, ", reg-num: %s, manuf: %s, call-sign: %s%s",
               a->reg_num, a->manufact[0] ? a->manufact : "?", a->call_sign[0] ? a->call_sign : "?",
               (*g_data.p_is_military)(addr, NULL) ? ", Military" : "");
  return (buf);
}

/**
 * Return some extra info on entering / leaving planes.
 * Not effective unless `--debug g` (or `--debug G`) is used.
 */
const char *aircraft_extra_info (const aircraft *a)
{
  static char buf [1000];
  char       *p   = buf;
  const char *end = p + sizeof(buf) - 1;

  if (!(Modes.debug & DEBUG_GENERAL))
     return ("");

  p += snprintf (p, end - p, ".\n              messages: %u", a->messages);

  if (a->AC_flags & MODES_ACFLAGS_CATEGORY_VALID)
       p += snprintf (p, end - p, ", category: 0x%02X", a->category);
  else p += snprintf (p, end - p, ", category: " NONE_STR);

  if (a->AC_flags & MODES_ACFLAGS_SQUAWK_VALID)
       p += snprintf (p, end - p, ", squawk: 0x%04X", a->identity);
  else p += snprintf (p, end - p, ", squawk: " NONE_STR);

  p += snprintf (p, end - p, "\n              AC_flags: %s",
                 aircraft_AC_flags(a->AC_flags));

  return (buf);
}

/**
 * Initialise a `filter` for `aircraft_match()`.
 */
bool aircraft_match_init (const char *arg)
{
  char *s, *spec = strdup (arg);
  bool  legal;

  if (!spec)
     return (false);

  strupr (spec);
  g_data.icao_spec = spec;

  if (*spec == '!')
  {
    g_data.icao_invert = true;
    spec++;
  }

  g_data.icao_filter = mg_str (spec);
  legal = mg_match (g_data.icao_filter, mg_str("*"), NULL);

  for (s = spec; *s; s++)
  {
    if (*s != '*' && hex_digit_val(*s) == -1)
       legal = false;
  }

  TRACE ("Argments '%.*s' used as ICAO-filter. legal: %d, invert: %d\n",
         (int)g_data.icao_filter.len, g_data.icao_filter.buf, legal, g_data.icao_invert);

  if (!legal)
     LOG_STDERR ("filter: '%s' is not legal.\n", arg);
  return (legal);
}

/**
 * Match the ICAO-address in `a` against `g_data.icao_filter`.
 */
bool aircraft_match (uint32_t a)
{
  char addr [10];
  int  rc;

  if (!g_data.icao_spec)  /* Match any ICAO address */
     return (true);

  assert (g_data.icao_filter.len > 0);

  snprintf (addr, sizeof(addr), "%06X", a);
  rc = mg_match (mg_str(addr), g_data.icao_filter, NULL);
  if (g_data.icao_invert)
     rc ^= true;

  if (!rc)
       Modes.stat.addr_filtered++;
  else DEBUG (DEBUG_GENERAL2, "0x%s matches%s0x%.*s\n",
              addr, g_data.icao_invert ? " !" : " ",
              (int)g_data.icao_filter.len, g_data.icao_filter.buf);

  return (rc);
}

/**
 * Sqlite3 interface functions
 */
static int sql_callback (void *cb_arg, int argc, char **argv, char **col_name)
{
  aircraft_info *a = (aircraft_info*) cb_arg;

  if (argc == 5 && mg_unhexn(argv[0], 6) == a->addr)
  {
    strcpy_s (a->reg_num,   sizeof(a->reg_num),   argv[1]);
    strcpy_s (a->manufact,  sizeof(a->manufact),  argv[2]);
    strcpy_s (a->type,      sizeof(a->type),      argv[3]);
    strcpy_s (a->call_sign, sizeof(a->call_sign), argv[4]);
  }
  (void) col_name;
  return (0);
}

static const aircraft_info *SQL_lookup_entry (uint32_t addr)
{
  static aircraft_info  a;
  const  aircraft_info *ret = NULL;
  char                  query [100];
  char                 *err_msg = NULL;
  uint32_t              addr2;
  int                   rc;

  if (!g_data.sql_db)
     return (NULL);

  memset (&a, '\0', sizeof(a));
  a.addr = addr;
  addr2  = addr;
  snprintf (query, sizeof(query), "SELECT * FROM aircrafts WHERE icao24='%06x';", addr);
  rc = sqlite3_exec (g_data.sql_db, query, sql_callback, &a, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("SQL error: rc: %d, %s\n", rc, err_msg);
    sqlite3_free (err_msg);
    if (rc == SQLITE_MISUSE)
       aircraft_exit (false);
  }
  else if (a.addr == addr2)
  {
    ret = &a;
  }
  return (ret);
}

static void sql_log (void *cb_arg, int err, const char *str)
{
#if defined(SQLITE_DQS) && (SQLITE_DQS > 0)
  /*
   * Ignore this warning
   */
  if (err == SQLITE_WARNING && str_startswith(str, "double-quoted string literal"))
     return;
#endif

  TRACE ("err: %d, %s\n", err, str);
  (void) cb_arg;
}

static bool sql_init (const char *what, int flags)
{
  int rc;

  if (!g_data.sql_db)     /* 1st time init */
     sqlite3_config (SQLITE_CONFIG_LOG, sql_log, NULL);

  if (!strcmp(what, "load"))
     return (true);

  rc = sqlite3_open_v2 (g_data.sql_file, &g_data.sql_db, flags, NULL);
  if (rc != SQLITE_OK)
  {
    TRACE ("Can't %s database: rc: %d, %s\n", what, rc, sqlite3_errmsg(g_data.sql_db));
    aircraft_exit (false);
    return (false);
  }
  return (true);
}

/**
 * Create the `g_data.sql_file` database with 5 columns.
 *
 * And make the CSV callback add the records into the `g_data.sql_file` file.
 */
static bool sql_create (void)
{
  char *err_msg = NULL;
  int   rc;

  if (!sql_init("create", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
     return (false);

  rc = sqlite3_exec (g_data.sql_db, "CREATE TABLE aircrafts (" DB_COLUMNS ");",
                     NULL, NULL, &err_msg);

  if (rc != SQLITE_OK &&
      strcmp(err_msg, "table aircrafts already exists")) /* ignore this "error" */
  {
    TRACE ("rc: %d, %s\n", rc, err_msg);
    sqlite3_free (err_msg);
    aircraft_exit (false);
    return (false);
  }
  return (true);
}

static bool sql_open (void)
{
  return sql_init ("open", SQLITE_OPEN_READONLY);
}

static bool sql_begin (void)
{
  char *err_msg = NULL;
  int   rc = sqlite3_exec (g_data.sql_db, "BEGIN;", NULL, NULL, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("rc: %d, %s\n", rc, err_msg);
    sqlite3_free (err_msg);
  }
  return (rc == SQLITE_OK);
}

static bool sql_end (void)
{
  char *err_msg = NULL;
  int   rc = sqlite3_exec (g_data.sql_db, "END;", NULL, NULL, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("rc: %d, %s\n", rc, err_msg);
    sqlite3_free (err_msg);
  }
  return (rc == SQLITE_OK);
}

/**
 * Add a CSV-record to the SQlite database.
 *
 * Use the `%m` format and `mg_print_esc()` to escape some possible control characters.
 * The `%m` format adds double-quotes around the strings. Hence single-quotes needs no
 * ESCaping.
 *
 * Another "feature" of Sqlite is that upper-case hex values are turned into lower-case
 * when 'SELECT * FROM' is done!
 */
static bool sql_add_entry (uint32_t num, const aircraft_info *ai)
{
  char   buf [sizeof(*ai) + sizeof(DB_INSERT) + 100];
  char  *values, *err_msg = NULL;
  int    rc;

  mg_snprintf (buf, sizeof(buf),
               DB_INSERT " ('%06x',%m,%m,%m,%m)",
               ai->addr,
               MG_ESC(ai->reg_num),
               MG_ESC(ai->manufact),
               MG_ESC(ai->type),
               MG_ESC(ai->call_sign));

  values = buf + sizeof(DB_INSERT) + 1;

  rc = sqlite3_exec (g_data.sql_db, buf, NULL, NULL, &err_msg);

  if (((num + 1) % 1000) == 0)
  {
    if (num >= 100000)
       printf ("%u\b\b\b\b\b\b", num);
    else if (num >= 10000)
       printf ("%u\b\b\b\b\b", num);
    else if (num >= 1000)
       printf ("%u\b\b\b\b", num);
  }

  if (rc != SQLITE_OK)
  {
    TRACE ("\nError at record %u: rc:%d, err_msg: %s\nvalues: '%s'\n", num, rc, err_msg, values);
    sqlite3_free (err_msg);
    return (false);
  }
  return (true);
}

static double get_signal (const aircraft *a)
{
  double sum = 0.0;
  int    i;
  bool   full_scale = (a->sig_idx >= (int)DIM(a->sig_levels));
  bool   half_scale = (a->sig_idx >= (int)DIM(a->sig_levels) / 2);

  if (full_scale)
  {
    for (i = 0; i < (int)DIM(a->sig_levels); i++)
        sum += a->sig_levels [i];
  }
  else if (half_scale)
  {
    for (i = 0; i < a->sig_idx; i++)
        sum += a->sig_levels [i];
  }
  return (10 * log10 (sum / 8 + 1.125E-5));
}

/*
 * Tar1090 want these JSON fields:
 */
static const char *json_alt   = "altitude";
static const char *json_speed = "speed";
static const char *json_vert  = "vert_rate";

void aircraft_fix_flightaware (void)
{
  if (Modes.web_page_is_FA)
  {
    /* But FlightAware wants these presumably:
     */
    json_alt   = "alt_baro";   /* the aircraft barometric altitude in feet */
    json_speed = "gs";         /* ground speed in knots */
    json_vert  = "geom_rate";  /* rate of change of geometric altitude, feet/minute */
  }
}

static char *append_flags (int flags, char *buf)
{
  char *p = buf;

  *p++ = '[';

  if (flags & MODES_ACFLAGS_SQUAWK_VALID)
     p += sprintf (p, "\"squawk\",");

  if (flags & MODES_ACFLAGS_CALLSIGN_VALID)
     p += sprintf (p, "\"callsign\",");

  if (flags & MODES_ACFLAGS_LATLON_VALID)
     p += sprintf (p, "\"lat\",\"lon\",");

  if (flags & MODES_ACFLAGS_ALTITUDE_VALID)
     p += sprintf (p, "\"%s\",", json_alt);

  if (flags & MODES_ACFLAGS_HEADING_VALID)
     p += sprintf (p, "\"track\",");

  if (flags & MODES_ACFLAGS_SPEED_VALID)
     p += sprintf (p, "\"%s\",", json_speed);

  if (flags & MODES_ACFLAGS_VERTRATE_VALID)
     p += sprintf (p, "\"%s\",", json_vert);

  if (flags & MODES_ACFLAGS_CATEGORY_VALID)
     p += sprintf (p, "\"category\",");

  p = strrchr (buf, ',');
  if (p)
     *p = '\0';
  *p++ = ']';
  *p = '\0';

  return (buf);
}

#define ASSERT_LEFT(_left, p, ret) do {                                      \
        if (left < _left) {                                                  \
           fprintf (stderr, "Internal error (line %u): left: %d, p: '%s'\n", \
                    __LINE__, left, p);                                      \
           fflush (stderr);                                                  \
           Modes.exit = g_data.internal_error = true;                        \
           return (ret);                                                     \
        }                                                                    \
      } while (0)

/**
 * Fill the JSON buffer `p` for one aircraft.
 * This assumes it could need upto `JSON_BUF_LEN/5` (4 kByte) for one aircraft.
 */
static size_t aircraft_make_one_json (const aircraft *a, bool extended_client, char *p, int left, uint64_t now)
{
  size_t size;
  char  *p_start = p;
  char   addr [10];
  int    altitude = a->altitude;
  int    speed    = a->speed;

  /* Convert units to metric if '--metric' was specified.
   * But an 'extended_client' wants altitude and speed in aeronatical units.
   */
  if (Modes.metric && !extended_client)
  {
    altitude = (int) (double) (a->altitude / 3.2828);
    speed    = (int) (1.852 * (double) a->speed);
  }

  snprintf (addr, sizeof(addr), "%s%06X",
            (a->addr & MODES_NON_ICAO_ADDRESS) ? "~" : "",
             a->addr & MODES_ICAO_ADDRESS_MASK);

  size = mg_snprintf (p, left, "{%m: %m", MG_ESC("hex"), MG_ESC(addr));
  p    += size;
  left -= (int)size;

  if (a->AC_flags & MODES_ACFLAGS_CALLSIGN_VALID)
  {
    size = mg_snprintf (p, left, ",%m:%m", MG_ESC("flight"), MG_ESC(a->call_sign));
    p    += size;
    left -= (int)size;
  }

  if (a->AC_flags & MODES_ACFLAGS_LATLON_VALID)
  {
    size = mg_snprintf (p, left, ",\"lat\":%f,\"lon\":%f,\"nucp\":%u,\"seen_pos\":%.1f",
                        a->position.lat, a->position.lon, a->pos_nuc, (now - a->seen_pos) / 1000.0);
    p    += size;
    left -= (int)size;
  }

  if ((a->AC_flags & MODES_ACFLAGS_AOG_VALID) && (a->AC_flags & MODES_ACFLAGS_AOG))
  {
    size = mg_snprintf (p, left, ",\"%s\":\"ground\"", json_alt);
    p    += size;
    left -= (int)size;
  }
  else if (a->AC_flags & MODES_ACFLAGS_ALTITUDE_VALID)
  {
    size = mg_snprintf (p, left, ",\"%s\":%d", json_alt, altitude);
    p    += size;
    left -= (int)size;
  }

  if (a->AC_flags & MODES_ACFLAGS_VERTRATE_VALID)
  {
    size  = mg_snprintf (p, left, ",\"%s\":%d", json_vert, a->vert_rate);
    p    += size;
    left -= (int)size;
  }

  if (a->AC_flags & MODES_ACFLAGS_HEADING_VALID)
  {
    size = mg_snprintf (p, left, ",\"track\":%d", (int)a->heading);
    p    += size;
    left -= (int)size;
  }

  if (a->AC_flags & MODES_ACFLAGS_SPEED_VALID)
  {
    size = mg_snprintf (p, left, ",\"%s\":%d", json_speed, speed);
    p    += size;
    left -= (int)size;
  }

  if (a->AC_flags & MODES_ACFLAGS_SQUAWK_VALID)
  {
    size = mg_snprintf (p, left, ",\"squawk\":\"%04x\"", a->identity);
    p    += size;
    left -= (int)size;
  }

  if (a->AC_flags & MODES_ACFLAGS_CATEGORY_VALID)
  {
    size = mg_snprintf (p, left, ",\"category\":\"%02X\"", a->category);
    p    += size;
    left -= (int)size;
  }

#if 0
  if (a->adsb_version >= 0)
  {
    size = mg_snprintf (p, left, ",\"version\":%d", a->adsb_version);
    p    += size;
    left -= (int)size;
  }
#endif

  ASSERT_LEFT (100, p_start, 0);

  if (extended_client)
  {
    char flag_buf [200];

    if (a->MLAT_flags)
    {
      size = mg_snprintf (p, left, ",\"mlat\":%s", append_flags(a->MLAT_flags, flag_buf));
      p    += size;
      left -= (int)size;
    }
    if (a->TISB_flags)
    {
      size = mg_snprintf (p, left, ",\"tisb\":%s", append_flags(a->TISB_flags, flag_buf));
      p    += size;
      left -= (int)size;
    }

#if 0
    if (a->addrtype != ADDR_ADSB_ICAO)
    {
      size = mg_snprintf (p, left, ",\"type\":\"%s\"", addrtype_enum_string(a->addrtype));
      p    += size;
      left -= (int)size;
    }
#endif

    double delta_t;

    if (now < a->seen_last)
         delta_t = 0.0;
    else delta_t = (double)(now - a->seen_last) / 1000.0;

    size = mg_snprintf (p, left, ",\"messages\": %u, \"seen\": %.1lf", a->messages, delta_t);
    p    += size;
    left -= (int)size;

    if (Modes.web_send_rssi)
    {
      size = mg_snprintf (p, left, ",\"rssi\": %.1lf", get_signal(a));
      p    += size;
      left -= (int)size;
    }
  }

  ASSERT_LEFT (3, p_start, 0);

  *p++ = '}';
  *p++ = ',';
  *p++ = '\n';

  return (p - p_start);
}

/**
 * Return a malloced JSON description of the active planes.
 * But only those whose latitude and longitude are valid.
 *
 * Since various Web-clients expects different elements in this returned
 * JSON array, add those which is appropriate for that Web-clients only.
 *
 * E.g. an extended web-client wants an empty array like this:
 * ```
 *  {
 *    "now": 1656176445, "messages": 1,
 *    "aircraft": []
 *  }
 * ```
 *
 * Or an array with 1 element like this:
 * ```
 * {
 *   "now:": 1656176445, "messages": 1,
 *   "aircraft": [{"hex":"47807D", "flight":"", "lat":60.280609, "lon":5.223715, "altitude":875, "track":199, "speed":96}]
 * }
 * ```
 *
 * \sa https://github.com/wiedehopf/readsb/blob/dev/README-json.md
 */
char *aircraft_make_json (bool extended_client, size_t *size_p)
{
  static uint32_t json_file_num = 0;
  struct timeval  tv_now;
  uint64_t        now;
  int             size;
  int             left = JSON_BUF_LEN;    /* 20kB; the initial buffer is incremented as needed */
  int             i, max;
  uint32_t        aircrafts = 0;
  char           *buf, *p;

  if (g_data.internal_error)
     return (NULL);

  *size_p = 0;

  buf = p = malloc (left);
  if (!buf)
     return (NULL);

  _gettimeofday (&tv_now, NULL);
  now = (1000 * (uint64_t)tv_now.tv_sec) + (tv_now.tv_usec / 1000);

  if (extended_client)
  {
    size = mg_snprintf (p, left,
                        "{\"now\":%lu.%03lu, \"messages\":%llu, \"aircraft\":\n",
                        tv_now.tv_sec, tv_now.tv_usec/1000, Modes.stat.messages_total);
    p    += size;
    left -= size;
  }

  *p++ = '[';   /* Start the json array */
  left--;

  max = smartlist_len (g_data.aircrafts);
  for (i = 0; i < max; i++)
  {
    const aircraft *a = smartlist_get (g_data.aircrafts, i);

    if (a->mode_AC_flags & MODEAC_MSG_FLAG)  /* skip any fudged ICAO records Mode A/C */
       continue;

    if (a->messages <= 1)   /* basic filter for bad decodes */
       continue;

#if 1
    if (!VALID_POS(a->position))
       continue;
#else
    if (!(a->AC_flags & MODES_ACFLAGS_LATLON_VALID))
       continue;
#endif

    size = aircraft_make_one_json (a, extended_client, p, left, now);
    p    += size;
    left -= size;

    aircrafts++;

    if (left < JSON_BUF_LEN/5)   /* Resize 'buf' if needed */
    {
      int used = p - buf;

      left += JSON_BUF_LEN;      /* Our increment; 20kB */
      buf = realloc (buf, used + left);
      if (!buf)
         return (NULL);

      p = buf + used;
    }
  }

  if (aircrafts > 0) /* ignore the last ",\n" */
  {
    p    -= 2;
    left += 2;
  }

  ASSERT_LEFT (3, buf, NULL);

  *p++ = ']';   /* Close the json array */

  if (extended_client)
  {
    *p++ = '\n';
    *p++ = '}';
  }
  *p = '\0';

  if (Modes.log && (Modes.debug & DEBUG_GENERAL2))
     fprintf (Modes.log, "\nJSON dump of file-number %u for %u aircrafts, extended_client: %d:\n%s\n\n",
              json_file_num++, aircrafts, extended_client, buf);

  *size_p = p - buf;
  return (buf);
}

/**
 * Called from `background_tasks()` via `aircraft_remove_stale()` 4 times a second.
 * But only does something once every `OUTLINE_PERIOD` (15 sec).
 *
 * \ref https://discussions.flightaware.com/t/comparing-tar1090-actual-range-plot-shapes/96965/2
 */
static bool aircraft_outline_generate (uint64_t now, const char *fname)
{
  static uint64_t prev_msec = 0;
  FILE  *f;
  int    i, hour;

  if (!g_data.outline_enable)
     return (false);

  if (!g_data.test_mode)
  {
    if ((now - prev_msec) < OUTLINE_PERIOD)   /* Period not passed */
       return (false);

    if (prev_msec == 0ULL)           /* return if first time called */
    {
      prev_msec = now;
      return (false);
    }
    prev_msec = now;
  }

  assert (fname);
  f = fopen (fname, "w+");
  if (!f)
  {
    g_data.outline_enable = false;
    return (false);
  }

  /* Check for maximum over last 24 ivals and current ival
   */
  dist_coords record [RANGEDIRS_BUCKETS];   /* record for this "hour" */
  memset (record, '\0', sizeof(record));

  for (hour = 0; hour < RANGEDIRS_IVALS; hour++)
  {
    for (i = 0; i < RANGEDIRS_BUCKETS; i++)
    {
      dist_coords curr = g_data.range_dirs [hour][i];

      if (curr.distance > record[i].distance)
         record[i] = curr;
    }
  }

  /* Print the new max-distance records in each direction
   */
  fputs ("{ \"actualRange\": { \"last24h\": { \"points\": [\n", f);
  for (i = 0; i < RANGEDIRS_BUCKETS; i++)
  {
    if (record[i].lat || record[i].lon)
    {
      const char *comma = (i < RANGEDIRS_BUCKETS - 1 ? "," : "");
      fprintf (f, "[%.4f,%.4f,%d]%s\n", record[i].lat, record[i].lon, record[i].alt, comma);
    }
  }
  fputs ("]}}}\n", f);

  if (g_data.test_mode)
     printf ("Wrote %ld bytes to '%s'\n", ftell(f), fname);
  fclose (f);
  return (true);
}

/*
 * Similar to "/data/aircraft.json", return data for a
 * "/data/outline.json" request.
 *
 * Returns a malloc()-ed string from `mg_file_read()`.
 */
char *aircraft_outline_json (const char *fname, size_t *size_p)
{
  char   *data = NULL;
  mg_str  mem;

  if (g_data.test_mode)
     assert (fname && g_data.outline_enable);

  if (!fname)
     fname = g_data.outline_json;

  *size_p = 0;

  if (!g_data.outline_enable)
     return (NULL);

  mem = mg_file_read (&mg_fs_posix, fname);
  if (!mem.buf)
  {
    LOG_STDERR ("Failed to open %s, errno: %d/%s\n", fname, errno, strerror(errno));
    return (NULL);
  }
  data = mem.buf;

  if (g_data.test_mode)
     printf ("Read %zd bytes outline-data from '%s'\n", mem.len, fname);

  data [mem.len] = '\0';
  *size_p = mem.len;
  return (data);
}

// Based on readsb's "update_range_histogram()"

static bool aircraft_outline_update (aircraft *a, uint64_t now)
{
  if (g_data.test_mode)
  {
    assert (a);
    assert (g_data.outline_enable);
    printf ("a: 0x%06X\n", a->addr);
  }

  if (!g_data.outline_enable)
     return (false);

  if (!a)
     return (false);

  // ... todo

  return (false);
}

/**
 * Returns the "/data/receiver.json" array to a Web-client which
 * describes the receiver:
 *  { "version": "0.4.9", "refresh": 1000, "history": 3, etc... }
 *
 * \param out     The buffer that should be written to.
 * \param in,out  On input, `*size_p` should be the max-size of `buf`.
 *                On output, `*size_p` is set to the length written to `buf`.
 */
char *aircraft_receiver_json (char *buf, size_t *size_p)
{
  int    len, history_size = DIM (g_data.json_history) - 1;
  size_t max_sz = *size_p;

  /* work out number of valid history entries
   */
  if (!g_data.json_history [history_size].buf)
     history_size = g_data.json_history_next;

  len = snprintf (buf, max_sz,
                  "{\"version\": \"%s\", "
                  "\"refresh\": %llu, "
                  "\"history\": %d, "
                  "\"lat\": %.8g, "        /* if 'Modes.home_pos_ok == false', this is 0. */
                  "\"lon\": %.8g, "        /* ditto */
                  "\"outlineJson\": %s}",  /* ranges available */
                  PROG_VERSION,
                  g_data.json_interval,
                  history_size,
                  Modes.home_pos.lat,
                  Modes.home_pos.lon,
                  g_data.outline_enable ? "true" : "false");

  *size_p = len;
  return (buf);
}

/**
 * Free a single aircraft `a` from the global list
 * `g_data.aircrafts`.
 */
static void aircraft_free (aircraft *a)
{
  free (a->SQL);
  free (a);
}

/**
 * Periodically search through the list of known Mode-S aircraft and tag them if this
 * Mode A/C matches their known Mode S Squawks or Altitudes (+/- 50 feet).
 *
 * A Mode S equipped aircraft may also respond to Mode A and Mode C SSR interrogations.
 * We can't tell if this is a Mode A or C, so scan through the entire aircraft list
 * looking for matches on Mode A (squawk) and Mode C (altitude). Flag in the Mode S
 * records that we have had a potential Mode A or Mode C response from this aircraft.
 *
 * If an aircraft responds to Mode A then it's highly likely to be responding to mode C
 * too, and vice verca. Therefore, once the mode S record is tagged with both a Mode A
 * and a Mode C flag, we can be fairly confident that this Mode A/C frame relates to that
 * Mode S aircraft.
 *
 * Mode C's are more likely to clash than Mode A's; there could be several aircraft
 * cruising at FL370, but it's less likely (though not impossible) that there are two
 * aircraft on the same squawk. Therefore, give precidence to Mode A record matches.
 *
 * \note It's theoretically possible for an aircraft to have the same value for Mode A
 *       and Mode C. Therefore we have to check BOTH A AND C for EVERY S.
 */
static void aircraft_update_mode_A (aircraft *a1)
{
  aircraft *a2;
  int       i, max = smartlist_len (g_data.aircrafts);

  for (i = 0; i < max; i++)
  {
    a2 = smartlist_get (g_data.aircrafts, i);

    if (!(a2->mode_AC_flags & MODEAC_MSG_FLAG))   /* skip any fudged ICAO records */
    {
      /* If both (a1) and (a2) have valid squawks
       */
      if ((a1->AC_flags & a2->AC_flags) & MODES_ACFLAGS_SQUAWK_VALID)
      {
        /* ...check for Mode-A == Mode-S Squawk matches
         */
        if (a1->identity == a2->identity)   /* If a 'real' Mode-S ICAO exists using this Mode-A Squawk */
        {
          a2->mode_A_count = a1->messages;
          a2->mode_AC_flags |= MODEAC_MSG_MODEA_HIT;
          a1->mode_AC_flags |= MODEAC_MSG_MODEA_HIT;
          if ((a2->mode_A_count > 0) &&
             ((a2->mode_C_count > 1) ||
             (a1->mode_AC_flags & MODEAC_MSG_MODEA_ONLY))) /* Allow Mode-A only matches if this Mode-A is invalid Mode-C */
          {
            a1->mode_AC_flags |= MODEAC_MSG_MODES_HIT;     /* flag this ModeA/C probably belongs to a known Mode S */
          }
        }
      }

      /* If both (a1) and (a2) have valid altitudes
       */
      if ((a1->AC_flags & a2->AC_flags) & MODES_ACFLAGS_ALTITUDE_VALID)
      {
        /* ... check for Mode-C == Mode-S Altitude matches
         */
        if (a1->altitude_C     == a2->altitude_C     ||  /* If a 'real' Mode-S ICAO exists at this Mode-C Altitude */
            a1->altitude_C     == a2->altitude_C + 1 ||  /* or this Mode-C - 100 ft */
            a1->altitude_C + 1 == a2->altitude_C)        /* or this Mode-C + 100 ft */
        {
          a2->mode_C_count = a1->messages;
          a2->mode_AC_flags |= MODEAC_MSG_MODEC_HIT;
          a1->mode_AC_flags |= MODEAC_MSG_MODEC_HIT;
          if (a2->mode_A_count > 0 && a2->mode_C_count > 1)
          {
            /* flag this ModeA/C probably belongs to a known Mode S
             */
            a1->mode_AC_flags |= (MODEAC_MSG_MODES_HIT | MODEAC_MSG_MODEC_OLD);
          }
        }
      }
    }
  }
}

static void aircraft_update_mode_S (aircraft *a)
{
  int flags = a->mode_AC_flags;

  if (flags & MODEAC_MSG_FLAG)  /* find any fudged ICAO records */
  {
    /* Clear the current A,C and S hit bits ready for this attempt
     */
    a->mode_AC_flags = flags & ~(MODEAC_MSG_MODEA_HIT | MODEAC_MSG_MODEC_HIT | MODEAC_MSG_MODES_HIT);
    aircraft_update_mode_A (a);  /* and attempt to match them with Mode-S */
  }
}

/**
 * If sum of all `a->global_dist_checks` counters equals
 * `Modes.stat.cpr_global_dist_checks` on every check during the
 * first minute of running, it means that `Modes.home_pos` is totally
 * wrong.
 *
 * Notify user and exit.
 */
static void aircraft_check_dist (uint64_t sum)
{
  static uint64_t last_sum = 0;

  if (last_sum > 0 && sum > 0 && Modes.stat.cpr_global_dist_checks > 0 &&
      (Modes.stat.cpr_global_dist_checks - sum) == 0ULL)
  {
    FILETIME  now;
    ULONGLONG elapsed;

    get_FILETIME_now (&now);
    elapsed = *(ULONGLONG*) &now - *(ULONGLONG*) &Modes.start_FILETIME;
    elapsed /= 10000000ULL;     /* from 100 nsec units to seconds */
    if (elapsed >= 50 && elapsed <= 80)
    {
      LOG_STDERR ("\7All aircrafts failed 'global-dist check' (%llu, sum: %llu).\n"
                  "Fix your \"homepos = lat,lon\" to continue.\n",
                  Modes.stat.cpr_global_dist_checks, sum);

      Modes.no_stats = true;
      modeS_signal_handler (0);  /* break out of main_data_loop()  */
    }
  }
  last_sum = sum;
}

/**
 * Called from `background_tasks()` 4 times per second.
 *
 * If we don't receive new nessages within `Modes.interactive_ttl`
 * milli-seconds, we remove the aircraft from the list.
 *
 * Also call `aircraft_update_mode_S(a)` and `aircraft_update_mode_A(a)`
 * in the same loop.
 */
void aircraft_remove_stale (uint64_t now)
{
  int      i, num, max = smartlist_len (g_data.aircrafts);
  uint64_t cpr_error_sum = 0;

  for (i = num = 0; i < max; i++)
  {
    aircraft *a = smartlist_get (g_data.aircrafts, i);
    int64_t   diff = (int64_t) (now - a->seen_last);

    /* Mark this plane for a "last time" view on next refresh?
     */
    if (a->show == A_SHOW_NORMAL && diff >= Modes.interactive_ttl - 1000)
    {
      a->show = A_SHOW_LAST_TIME;
    }
    else if (diff > Modes.interactive_ttl)
    {
      LOG_UNFOLLOW (a);

      /* Remove the element from the smartlist.
       * \todo
       * Perhaps copy it to a `aircraft_may_reenter` list?
       * Or leave it in the list with show-state as `A_SHOW_NONE`.
       */
      aircraft_free (a);
      smartlist_del (g_data.aircrafts, i);
      max--;
      continue;
    }

    if ((a->AC_flags & MODES_ACFLAGS_LATLON_VALID) && diff > Modes.interactive_ttl)
    {
      /* Position is too old and no longer valid
       */
      a->AC_flags &= ~(MODES_ACFLAGS_LATLON_VALID | MODES_ACFLAGS_LATLON_REL_OK);
    }

    aircraft_update_mode_S (a);

    if (a->AC_flags & MODES_ACFLAGS_LATLON_VALID)
       cpr_error_sum += a->global_dist_checks;
    num++;
  }

  if (num > 0 && !(Modes.net_active || Modes.net_only))
  {
    aircraft_check_dist (cpr_error_sum);
    aircraft_outline_update (NULL, now);
  }
  aircraft_outline_generate (now, g_data.outline_json);
}

/**
 * Set this aircraft's estimated distance to our home position.
 *
 * Assuming a constant good last heading and speed, calculate the
 * new position from that using the elapsed time.
 */
bool aircraft_set_est_home_distance (aircraft *a, uint64_t now)
{
  double dist_m, est_dist_m;
  double d_X, d_Y, d_S;
  double speed = (a->speed * 1852.0) / 3600.0;  /* knots -> m/sec */
  double heading, scale;
  pos_t  epos;
  bool   valid;

  if (!Modes.home_pos_ok || !(a->AC_flags & MODES_ACFLAGS_HEADING_VALID))
     return (false);

  if (speed <= SMALL_VAL || !(a->AC_flags & MODES_ACFLAGS_SPEED_VALID))
     return (false);

  if (!(a->AC_flags & MODES_ACFLAGS_LATLON_VALID))
     return (false);

  if (!VALID_POS(a->position_EST) || a->seen_pos_EST < a->seen_last)
     return (false);

  if (now - a->seen_last > 10000)     /* to old */
     return (false);

  epos = a->position_EST;

  /* Convert to radians. Ensure heading is
   * in range '[-Phi .. +Phi]'.
   */
  heading = a->heading;
  if (heading > 180.0)
       heading = M_PI * (heading - 360.0) / 180.0;
  else heading = (M_PI * heading) / 180.0;

  if (a->seen_pos_EST == now)
     now++;

  /* dist_m: meters traveled in 'd_S':
   */
  d_S = (double) (now - a->seen_pos_EST) / 1E3;
  dist_m = speed * d_S;

  d_X = dist_m * sin (heading);
  d_Y = dist_m * cos (heading);

  scale = (2 * M_PI * 360.0) / EARTH_RADIUS;
  epos.lon += d_X * scale;
  epos.lat += d_Y * scale;

  est_dist_m = geo_great_circle_dist (&epos, &Modes.home_pos);

  /* difference < 10%
   */
  valid = fabs (est_dist_m - a->distance) / a->distance < 0.1;

  if ((Modes.debug & DEBUG_PLANE) && a->addr == Modes.a_follow)
     LOG_FILEONLY ("%06X: speed: %5.1lf, heading: %+5.1lf, d_X: %+5.0lf, d_Y: %+5.0lf, "
                   "dist_m: %5.0lf, d_S: %.3lf, est_dist_m: %4.2lf km%c\n",
                   a->addr, speed, 180.0 * heading / M_PI, d_X, d_Y, dist_m, d_S,
                   est_dist_m / 1E3, valid ? ' ' : '!');

  if (valid)
  {
    a->position_EST = epos;
    a->distance_EST = est_dist_m;
    a->seen_pos_EST = now;
  }
  return (valid);
}


/**
 * Called from show_statistics() to print statistics collected here.
 */
void aircraft_show_stats (void)
{
  LOG_STDOUT ("! \n");
  LOG_STDOUT ("Aircrafts statistics:\n");
  interactive_clreol();

  LOG_STDOUT (" %8llu unique aircrafts:\n", Modes.stat.unique_aircrafts);
  LOG_STDOUT (" %8llu in CSV-file.\n", Modes.stat.unique_aircrafts_CSV);
  LOG_STDOUT (" %8llu in BIN-file.\n", Modes.stat.unique_aircrafts_BIN);
  LOG_STDOUT (" %8llu in SQL-file.\n", Modes.stat.unique_aircrafts_SQL);
  interactive_clreol();

#if 0   /**\todo print details on unique aircrafts */
  char comment [100];
  int  i, max = smartlist_len (g_a_unique);

  for (i = num = 0; i < max; i++)
  {
    const aircraft *a = smartlist_get (g_a_unique, i);
  }
#endif
}

/**
 * Config-callbacks:
 */
bool aircraft_set_csv (const char *arg)
{
  free (g_data.csv_file);
  g_data.csv_file = strdup (arg);
  return (true);
}

bool aircraft_set_url (const char *arg)
{
  free (g_data.zip_url);
  g_data.zip_url = strdup (arg);
  return (true);
}

/**
 * Called from dump1090.c to set defaults in this module.
 * Before any .cfg file is parsed.
 */
void aircraft_pre_init (void)
{
  char *dir;

  assert (Modes.where_am_I);
  assert (g_data.csv_file == NULL);
  assert (g_data.zip_url == NULL);

  g_data.csv_file = mg_mprintf ("%s\\%s", Modes.where_am_I, AIRCRAFT_DATABASE_CSV);
  g_data.zip_url  = strdup (AIRCRAFT_DATABASE_URL);

  g_data.outline_enable = true;
  g_data.outline_json = mg_mprintf ("%s\\%s", Modes.tmp_dir, OUTLINE_JSON);

  dir = dirname (g_data.outline_json);
  if (!CreateDirectory(dir, 0) && GetLastError() != ERROR_ALREADY_EXISTS)
     LOG_STDERR ("'CreateDirectory(\"%s\")' failed; %s.\n", dir, win_strerror(GetLastError()));
}

/**
 * Called from dump1090.c to initialize this module based on .cfg values etc.
 */
bool aircraft_init (void)
{
  g_data.test_mode = test_contains (Modes.tests, "aircraft") && !g_data.test_done;
  g_data.test_done = true;

  g_data.a_sort = INTERACTIVE_SORT_NONE;
  g_data.json_interval = 1000;

#if defined(USE_BIN_FILES)
  g_data.p_get_country = aircraft_get_country2;
  g_data.p_is_military = aircraft_is_military2;
#else
  g_data.p_get_country = aircraft_get_country;
  g_data.p_is_military = aircraft_is_military;
#endif

  g_data.aircrafts = smartlist_new();
  if (!g_data.aircrafts)
  {
    LOG_STDERR ("`smartlist_new()` failed.\n");
    return (false);
  }

  aircraft_SQL_set_name();

  if (strcmp(g_data.csv_file, "NUL") && (g_data.zip_url || Modes.update))
  {
    if (!aircraft_update_CSV(g_data.csv_file, g_data.zip_url, CSV_MAX_AGE))
       return (false);
    if (!aircraft_load_CSV() && !Modes.update)
       return (false);
  }
  return (true);
}

/**
 * Called to:
 *  \li Close the Sqlite interface.
 *  \li And possibly free memory allocated here (if called from `modeS_exit()`
 *      with `free_aircrafts == true`).
 */
void aircraft_exit (bool free_aircrafts)
{
  if (g_data.sql_db)
     sqlite3_close (g_data.sql_db);
  g_data.sql_db = NULL;

  if (!free_aircrafts)
     return;

  /* Remove all active aircrafts from the list.
   * And free the list itself.
   */
  if (g_data.aircrafts)
  {
    smartlist_wipe (g_data.aircrafts, (smartlist_free_func)aircraft_free);
    g_data.aircrafts = NULL;
  }

  free_CSV_BIN_records();

  free (g_data.csv_file);
  free (g_data.sql_file);
  free (g_data.icao_spec);
  free (g_data.zip_url);
  free (g_data.outline_json);

  g_data.csv_file = g_data.sql_file = g_data.icao_spec = g_data.zip_url = NULL;
  g_data.outline_json = NULL;

#if defined(USE_BIN_FILES)
  free (Modes.bin.aircrafts_bin);
  Modes.bin.aircrafts_bin = NULL;
#endif
}

/**
 * We got a new CPR position from `mm->raw_longitude` and/or `mm->raw_latitude`
 * in `decode_extended_squitter()`.
 *
 * Handle it by calling the appropriate cpr.c function.
 */
static void aircraft_update_pos (aircraft *a, modeS_message *mm, uint64_t now)
{
  int      CPR_result = -1;
  int      max_elapsed;
  pos_t    new_pos = { 0.0, 0.0 };
  unsigned new_nuc = 0;

  if (mm->AC_flags & MODES_ACFLAGS_AOG)
       Modes.stat.cpr_surface++;
  else Modes.stat.cpr_airborne++;

  if (mm->AC_flags & MODES_ACFLAGS_AOG)
  {
    /* Surface: 25 seconds if >25kt or speed unknown, 50 seconds otherwise
     */
    if ((mm->AC_flags & MODES_ACFLAGS_SPEED_VALID) && mm->velocity <= 25)
         max_elapsed = 50000;
    else max_elapsed = 25000;
  }
  else
  {
    /* Airborne: 10 seconds
     */
    max_elapsed = 10000;
  }

  if (mm->AC_flags & MODES_ACFLAGS_LLODD_VALID)
  {
    a->odd_CPR_nuc  = mm->nuc_p;
    a->odd_CPR_lat  = mm->raw_latitude;
    a->odd_CPR_lon  = mm->raw_longitude;
    a->odd_CPR_time = now;
  }
  else
  {
    a->even_CPR_nuc  = mm->nuc_p;
    a->even_CPR_lat  = mm->raw_latitude;
    a->even_CPR_lon  = mm->raw_longitude;
    a->even_CPR_time = now;
  }

  /* If we have enough recent data, try global CPR
   */
  if (((mm->AC_flags | a->AC_flags) & MODES_ACFLAGS_LLEITHER_VALID) == MODES_ACFLAGS_LLBOTH_VALID &&
      abs((int)(a->even_CPR_time - a->odd_CPR_time)) <= max_elapsed)
  {
    CPR_result = cpr_do_global (a, mm, now, &new_pos, &new_nuc);

    if (CPR_result == -2)
    {
      if (mm->AC_flags & MODES_ACFLAGS_FROM_MLAT)
         CPR_TRACE ("%06X: failure from MLAT\n", a->addr);

      /* Global CPR failed because the position produced implausible results.
       * This is bad data. Discard both odd and even messages and wait for a fresh pair.
       * Also disable aircraft-relative positions until we have a new good position (but don't discard the
       * recorded position itself)
       */
      Modes.stat.cpr_global_bad++;
      a->AC_flags &= ~(MODES_ACFLAGS_LATLON_REL_OK | MODES_ACFLAGS_LLODD_VALID | MODES_ACFLAGS_LLEVEN_VALID);

      /* Also discard the current message's data as it is suspect -
       * we don't want to update any of the aircraft state from this.
       */
      mm->AC_flags &= ~(MODES_ACFLAGS_LATLON_VALID   |
                        MODES_ACFLAGS_LLODD_VALID    |
                        MODES_ACFLAGS_LLEVEN_VALID   |
                        MODES_ACFLAGS_ALTITUDE_VALID |
                        MODES_ACFLAGS_SPEED_VALID    |
                        MODES_ACFLAGS_HEADING_VALID  |
                        MODES_ACFLAGS_NSEWSPD_VALID  |
                        MODES_ACFLAGS_VERTRATE_VALID |
                        MODES_ACFLAGS_AOG_VALID      |
                        MODES_ACFLAGS_AOG);
      return;
    }

    if (CPR_result == -1)
    {
      if (mm->AC_flags & MODES_ACFLAGS_FROM_MLAT)
         CPR_TRACE ("%06X: skipped from MLAT\n", a->addr);

      /* No local reference for surface position available, or the two messages crossed a zone.
       * Nonfatal, try again later.
       */
      Modes.stat.cpr_global_skipped++;
    }
    else
    {
      Modes.stat.cpr_global_ok++;
    }
  }

  /* Otherwise try relative CPR
   */
  if (CPR_result == -1)
  {
    CPR_result = cpr_do_local (a, mm, now, &new_pos, &new_nuc);

    if (CPR_result == -1)
    {
      Modes.stat.cpr_local_skipped++;
    }
    else
    {
      Modes.stat.cpr_local_ok++;
      if (a->AC_flags & MODES_ACFLAGS_LATLON_REL_OK)
           Modes.stat.cpr_local_aircraft_relative++;
      else Modes.stat.cpr_local_receiver_relative++;
      mm->AC_flags |= MODES_ACFLAGS_REL_CPR_USED;
    }
  }

  if (CPR_result == 0)   /* okay */
  {
    /* If we sucessfully decoded, back copy the results to 'mm'
     * so that we can print them in list output
     */
    mm->AC_flags |= MODES_ACFLAGS_LATLON_VALID;
    mm->position = new_pos;

    /* Update aircraft state
     */
    a->AC_flags |= (MODES_ACFLAGS_LATLON_VALID | MODES_ACFLAGS_LATLON_REL_OK);
    a->position = new_pos;
    a->pos_nuc  = new_nuc;
    a->distance = geo_great_circle_dist (&Modes.home_pos, &a->position);
    a->seen_pos = a->seen_last;
/*  aircraft_update_histogram (a); */ /**< \todo Add periodic histogram */
  }
}

/**
 * Handle a new message `mm`.
 * Find and modify the aircraft state or create a new aircraft.
 *
 * Called from `modeS_user_message()`.
 */
aircraft *aircraft_update_from_message (modeS_message *mm)
{
  aircraft *a;
  uint64_t  now = MSEC_TIME();
  bool      is_new;      /* a new aircraft from this message? */

  a = aircraft_find_or_create (mm->addr, now, &is_new);
  if (!a)
     return (NULL);

  LOG_FOLLOW (a);

  if (mm->sig_level > 0)
  {
    a->sig_levels [a->sig_idx++] = mm->sig_level;
    a->sig_idx &= DIM(a->sig_levels) - 1;
  }

  a->seen_last = now;
  a->from_SBS  = mm->SBS_in;
  a->messages++;

  if (is_new)
  {
    int i;

   /* set initial signal-levels
    */
    for (i = 0; i < DIM(a->sig_levels); i++)
        a->sig_levels [i] = 1E-5;
    a->sig_idx = 0;

   /* copy the first message so we can use it later.
    */
    a->first_msg = *mm;

    /* mm->msg_type 32 is used to represent Mode A/C. These values can never change,
     * so set them once here during initialisation, and don't bother to set them every
     * time this ModeA/C is received again in the future
     */
    if (mm->msg_type == 32)
    {
      int modeC = mode_A_to_mode_C (mm->identity | mm->flight_status);

      a->mode_AC_flags = MODEAC_MSG_FLAG;
      if (modeC < -12)
         a->mode_AC_flags |= MODEAC_MSG_MODEA_ONLY;
      else
      {
        mm->altitude = modeC * 100;
        mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;
      }
    }
  }

  /* if the Aircraft has landed or taken off since the last message, clear the even/odd CPR flags
   */
  if ((mm->AC_flags & MODES_ACFLAGS_AOG_VALID) && ((a->AC_flags ^ mm->AC_flags) & MODES_ACFLAGS_AOG))
     a->AC_flags &= ~(MODES_ACFLAGS_LLBOTH_VALID | MODES_ACFLAGS_AOG);

  /* No CPR processing for SBS-IN messages.
   */
  if (mm->SBS_in)
  {
    if (mm->AC_flags & MODES_ACFLAGS_LATLON_VALID)
    {
      a->seen_pos_EST = now;
      a->position     = mm->position;
      a->position_EST = a->position;
      a->distance_ok  = true;
      a->distance = geo_great_circle_dist (&Modes.home_pos, &a->position);
    }
  }
  else
  {
    /* Do the CPR processing in `aircraft_update_pos()`.
     */
    if (mm->AC_flags & MODES_ACFLAGS_LLEITHER_VALID)
       aircraft_update_pos (a, mm, now);
  }

  /* If a (new) CALLSIGN has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_CALLSIGN_VALID)
  {
    memcpy (a->call_sign, mm->flight, sizeof(a->call_sign));
#if 1
    if (!Modes.sbs_in && !memcmp(a->call_sign, "TEST1234", 8))
       LOG_FILEONLY ("TEST1234: AC_flags: 0x%08X\n", mm->AC_flags);
#endif
  }

  /* If a (new) ALTITUDE has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_ALTITUDE_VALID)
  {
    if (a->mode_C_count                                 /* if we've a 'mode_C_count' already */
        && a->altitude       != mm->altitude            /* and Altitude has changed */
/*      && a->altitude_C     != mm->altitude_C + 1 */   /* and Altitude not changed by +100 feet */
/*      && a->altitude_C + 1 != mm->altitude_C */       /* and Altitude not changes by -100 feet */
       )
    {
      a->mode_C_count = 0;               /* zero the hit count */
      a->mode_AC_flags &= ~MODEAC_MSG_MODEC_HIT;
    }

    /* If we received an altitude in a (non-MLAT) DF17/18 squitter recently, ignore
     * DF0/4/16/20 altitudes as single-bit errors can attribute them to the wrong
     * aircraft
     */
    if (((a->AC_flags & ~a->MLAT_flags) & MODES_ACFLAGS_ALTITUDE_VALID) &&
        (now - a->seen_altitude) < 15000 &&
        ((a->AC_flags & ~a->MLAT_flags) & MODES_ACFLAGS_LATLON_VALID) &&
        (now - a->seen_pos) < 15000 &&
        mm->msg_type != 17 &&
        mm->msg_type != 18)
    {
      Modes.stat.suppressed_altitude_messages++;
    }
    else
    {
      a->altitude      = mm->altitude;
      a->altitude_C    = (mm->altitude + 49) / 100;
      a->seen_altitude = now;

      /* Reporting of HAE and baro altitudes is mutually exclusive
       * so if we see a baro altitude, assume the HAE altitude is invalid
       * we will recalculate it from baro + HAE delta below, where possible
       */
      a->AC_flags &= ~MODES_ACFLAGS_ALTITUDE_HAE_VALID;
    }
  }

  /* If a (new) HAE altitude has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_ALTITUDE_HAE_VALID)
  {
    a->altitude_HAE = mm->altitude_HAE;

    /* Reporting of HAE and baro altitudes is mutually exclusive
     * if you have both, you're meant to report baro and a HAE delta,
     * so if we see explicit HAE then assume the delta is invalid too
     */
    a->AC_flags &= ~(MODES_ACFLAGS_ALTITUDE_VALID | MODES_ACFLAGS_HAE_DELTA_VALID);
  }

  /* If a (new) HAE/barometric difference has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_HAE_DELTA_VALID)
  {
    a->HAE_delta = mm->HAE_delta;

    /* Reporting of HAE and baro altitudes is mutually exclusive
     * if you have both, you're meant to report baro and a HAE delta,
     * so if we see a HAE delta then assume the HAE altitude is invalid
     */
    a->AC_flags &= ~MODES_ACFLAGS_ALTITUDE_HAE_VALID;
  }

  /* If a (new) SQUAWK has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_SQUAWK_VALID)
  {
    if (a->identity != mm->identity)
    {
      a->mode_A_count = 0;   /* Squawk has changed, so zero the hit count */
      a->mode_AC_flags &= ~MODEAC_MSG_MODEA_HIT;
    }
    a->identity = mm->identity;
  }

  /* If a (new) HEADING has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_HEADING_VALID)
  {
    a->heading = mm->heading;
    a->AC_flags |= MODES_ACFLAGS_HEADING_VALID;

    if (a->AC_flags & MODES_ACFLAGS_LATLON_VALID)
       LOG_BEARING (a);
  }

  /* If a (new) SPEED has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_SPEED_VALID)
  {
    a->speed = mm->velocity;
    a->seen_speed = now;
  }

  /* If a (new) Vertical Descent rate has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_VERTRATE_VALID)
     a->vert_rate = mm->vert_rate;

  /* If a (new) category has been received, copy it to the aircraft structure
   */
  if (mm->AC_flags & MODES_ACFLAGS_CATEGORY_VALID)
     a->category = mm->category;

  /* Update the aircrafts `a->AC_flags` to reflect the newly received `mm->AC_flags`
   */
  a->AC_flags |= mm->AC_flags;

  /* If we have a baro altitude and a HAE delta from baro, calculate the HAE altitude
   */
  if ((a->AC_flags & MODES_ACFLAGS_ALTITUDE_VALID) && (a->AC_flags & MODES_ACFLAGS_HAE_DELTA_VALID))
  {
    a->altitude_HAE = a->altitude + a->HAE_delta;
    a->AC_flags |= MODES_ACFLAGS_ALTITUDE_HAE_VALID;
  }

  /* Update `a->MLAT_flags`. These indicate which bits in `a->AC_flags`
   * were last set based on a MLAT-derived message.
   */
  if (mm->AC_flags & MODES_ACFLAGS_FROM_MLAT)
       a->MLAT_flags = (a->MLAT_flags & a->AC_flags) | mm->AC_flags;
  else a->MLAT_flags = (a->MLAT_flags & a->AC_flags) & ~mm->AC_flags;

  /* Same for TIS-B:
   */
  if (mm->AC_flags & MODES_ACFLAGS_FROM_TISB)
       a->TISB_flags = (a->TISB_flags & a->AC_flags) | mm->AC_flags;
  else a->TISB_flags = (a->TISB_flags & a->AC_flags) & ~mm->AC_flags;

  if (mm->msg_type == 32)
  {
    int flags = a->mode_AC_flags;

    if ((flags & (MODEAC_MSG_MODEC_HIT | MODEAC_MSG_MODEC_OLD)) == MODEAC_MSG_MODEC_OLD)
    {
      /*
       * This Mode-C doesn't currently hit any known Mode-S, but it used to because MODEAC_MSG_MODEC_OLD is set.
       * So the aircraft it used to match has either changed altitude, or gone out of our receiver distance.
       *
       * We've now received this Mode-A/C again, so it must be a new aircraft. It could be another aircraft
       * at the same Mode-C altitude, or it could be a new aircraft with a new Mods-A squawk.
       *
       * To avoid masking this aircraft from the interactive display, clear the MODEAC_MSG_MODES_OLD flag
       * and set messages to 1;
       */
      a->mode_AC_flags = flags & ~MODEAC_MSG_MODEC_OLD;
      a->messages = 1;
    }
  }
  return (a);
}

