/**
 * \file    airports.c
 * \ingroup Main
 * \brief   Handling of airport data and cached flight-information from .CSV files.
 *          Uses the ADSB-LOL API to get live "departure" and "destination" information. <br>
 *          \sa https://api.adsb.lol
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>
#include <mbstring.h>

#include "misc.h"
#include "interactive.h"
#include "routes.h"
#include "smartlist.h"
#include "speech.h"
#include "airports.h"

#define API_SERVICE_URL     "https://vrs-standing-data.adsb.lol/routes/%.2s/%s.json"
#define API_SERVICE_503     "<html><head><title>503 Service Temporarily Unavailable"

#define API_SERVICE_URL2    "https://adsb.im/api/0/routeset"

#define API_AIRPORT_IATA    "\"_airport_codes_iata\": "    /* what to look for in response */
#define API_AIRPORT_ICAO    "\"airport_codes\": "          /* todo: look for these ICAO codes too */
#define API_SLEEP_MS        100                            /* Sleep() granularity */
#define API_MAX_AGE         (10 * 60 * 10000000ULL)        /* 10 min in 100 nsec units */
#define API_CACHE_PERIOD    (5 * 60 * 1000)                /* Save the cache every 5 min */
#define ICAO_UNKNOWN        0xFFFFFFFF                     /* mark an unused ICAO address */

/**
 * \enum airport_t
 * The source type for an \ref airport or \ref flight_info record.
 */
typedef enum airport_t {
        AIRPORT_CSV = 1,
        AIRPORT_API_LIVE,     /* == 2 */
        AIRPORT_API_CACHED,
        AIRPORT_API_EXPIRED,  /* == 4 */
        AIRPORT_API_PENDING,
        AIRPORT_API_DEAD      /* == 6 (possibly caused by a "HTTP 404") */
      } airport_t;

/**
 * \typedef airport
 *
 * Describes an airport. Data can be from these sources:
 *  \li A .CSV-file         (`Mode.airport_db == "airport-codes.csv"`)
 *  \li A live API request.
 *  \li A cached API request (`Mode.airport_api_cache == "%TEMP%\\dump1090\\airport-api-cache.csv"`).
 *
 * These are NOT on the same order as in `airport-codes.csv`. <br>
 * CSV header:
 * `# ICAO, IATA, Full_name, Continent, Location, Longitude, Latitude`
 */
typedef struct airport {
        char       ICAO [10];       /**< ICAO code (4 characters) */
        char       IATA [10];       /**< IATA code  (3 characters) */
        char       continent [3];   /**< ISO-3166 2 letter continent code */
        char       location  [30];  /**< Location or city */
        char       full_name [50];  /**< Full name */
        pos_t      pos;             /**< Latitude & longitude */
        airport_t  type;            /**< Source of this record */
      } airport;

/**
 * \typedef airport_freq
 *
 * Data for a single airport frequency.
 * Also contains a link to a `g_data.airports_priv.airports` node.
 */
typedef struct airport_freq {
        char           freq_id [3];
        char           ident [10];
        double         frequency;
        const airport *airport;
      } airport_freq;

/*
 * Handling of "Flight Information".
 */

/**
 * \typedef flight_info
 *
 * A flight-information record from a live or cached API request.
 */
typedef struct flight_info {
        char                call_sign   [10];  /**< call-sign for this flight */
        char                departure   [30];  /**< IATA departure airport for this flight */
        char                destination [30];  /**< IATA destination airport for this flight */
        uint32_t            ICAO_addr;         /**< ICAO address of plane */
        airport_t           type;              /**< the type of this record */
        FILETIME            created;           /**< time when this record was created and requested (UTC) */
        FILETIME            responded;         /**< time when this record had a response (UTC) */
        int                 http_status;       /**< the HTTP status-code (or 0) */
        bool                done_trace;        /**< relax the `API_TRACE()` for this record */
        bool                normalized;        /**< the `call_sign[]` was normalized */
      } flight_info;

/**
 * \typedef flight_info_stats
 *
 * Statistics for flight-info handling.
 */
typedef struct flight_info_stats {
        uint32_t  total;
        uint32_t  live;
        uint32_t  pending;
        uint32_t  cached;
        uint32_t  dead;
        uint32_t  expired;
        uint32_t  unknown;
      } flight_info_stats;

/**
 * \typedef airports_stats
 *
 * Statistics for airports + routes handling.
 */
typedef struct airports_stats {
        uint32_t  CSV_numbers;         /**< Count of CSV records in Modes.airport_db file */
        uint32_t  CSV_num_ICAO;        /**< of which was ICAO records (there are most dominant) */
        uint32_t  CSV_num_IATA;        /**< or which was IATA records */
        uint32_t  API_requests_sent;   /**< Count of requests sent in API-thread  */
        uint32_t  API_response_recv;   /**< Count of ANY responses received in API-thread */
        uint32_t  API_service_404;     /**< Count of "404 Not found" responses */
        uint32_t  API_service_503;     /**< Count of "503 Service Temporarily Unavailable" responses */
        uint32_t  API_added_CSV;       /**< Count of cached flight-info record in `g_data.flight_info`. */
        uint32_t  API_used_CSV;        /**< Count of cached flight-info record that was used in a lookup. */
        uint32_t  routes_records_used;
      } airports_stats;

/**
 * \typedef airports_names
 *
 * Mapping structure for mapping ICAO to IATA names.
 */
typedef struct airport_names {
        const char *ICAO;
        const char *IATA;
      } airport_names;

/**
 * \typedef airports_priv
 *
 * Private data for this module.
 */
typedef struct airports_priv {
        smartlist_t      *airports;       /**< smartlist of airports */
        airport          *airport_CSV;    /**< List of airports sorted on ICAO address. From CSV-file only */
        char            **IATA_to_ICAO;   /**< List of IATA to ICAO airport codes sorted on IATA address */

        smartlist_t      *flight_info;    /**< A smartlist of flight information */
        airport_freq     *freq_CSV;       /**< List of airport frequency information. Not yet */
        CSV_context       csv_ctx;        /**< Structure for the CSV parser */
        airports_stats    ap_stats;       /**< Accumulated statistics for airports */
        flight_info_stats fs_stats;       /**< Accumulated statistics for flight-info */
        uintptr_t         thread_hnd;     /**< Thread-handle from `_beginthreadex()` */
        HANDLE            thread_event;   /**< Thread-event for `API_thread_func()` to signal on */
        unsigned          thread_id;      /**< Thread-ID from `_beginthreadex()` */
        bool              do_trace;       /**< Use `API_TRACE()` macro? */
        bool              do_trace_LOL;   /**< or use `API_TRACE_LOL()` macro? */
        bool              no_db;          /**< Running w/o `Modes.airport_db' */
        bool              init_done;
        bool              test_mode;
        uint32_t          last_rc;

       /**
        * Only effective if `USE_BIN_FILES` is defined.
        */
        mg_file_path      routes_bin;
        mg_file_path      aircrafts_bin;
        mg_file_path      airports_bin;

        /**
         * \todo
         * Add the WinInet handles here for faster loading of it's functions.
         *
         * HINTERNET h1;
         * HINTERNET h2;
         */
     }  airports_priv;

static void         patch_call_signs_tests (void);
static void         airport_API_test_1 (void);
static void         airport_API_test_2 (void);
static void         airport_CSV_test_1 (void);
static void         airport_CSV_test_2 (void);
static void         airport_CSV_test_3 (void);
static void         airport_CSV_test_4 (void);
static void         airport_loc_test_1 (void);
static void         airport_loc_test_2 (void);
static void         airport_normalize_test (void);
static void         airport_print_header (unsigned line, bool use_usec);
static void         locale_test (void);
static void         flight_info_exit (FILE *f);
static flight_info *flight_info_create (const char *call_sign, uint32_t addr, airport_t type);
static bool         flight_info_write (FILE *file, const flight_info *f);
static void         flight_stats_now (flight_info_stats *stats);

static const char   *usec_fmt;
static airports_priv g_data;

#if defined(USE_BIN_FILES)
  static route_record2 *route_records;
  static size_t         route_records_num;

  // static airport_record *airports_records;
  // static size_t          airports_records_num;

  // static aircraft_record *aircraft_records;
  // static size_t           aircraft_records_num;
#endif

/**
 * Used in `airport_API_test_1()` and `airport_API_test_2()`.
 *
 * These are terribly outdated. Hence these are patched to 6 random call-signs
 * from the generated '%TEMP%/dump1090/standing-data/results/routes.bin'.
 * \ref patch_call_signs_tests()
 */
static char *call_signs_tests[] = {
            "AAL292",  "SK293",
            "TY15",    "WIF17T",
            "CFG2092", "NOZ8LE"
          };

/*
 * Used in `airport_CSV_test_X()` before calling `airport_print_rec()`
 */
#define AIRPORT_PRINT_HEADER(use_usec) \
        airport_print_header (__LINE__, use_usec)

/*
 * For generic trace in `--debug` + `--test` modes.
 */
#define API_TRACE(fmt, ...) do {                             \
                              if (g_data.do_trace)           \
                                 API_trace (__LINE__, fmt,   \
                                            ## __VA_ARGS__); \
                            } while (0)

/**
 * \def API_TRACE_LOL(req_resp, num, str, f)
 *  Print to `Modes.log` for ADSB-LOL API tracing but
 *  only if the `--debug a` option was used.
 */
#define API_TRACE_LOL(req_resp, num, str, f)        \
        do {                                        \
          if (g_data.do_trace_LOL && Modes.log)     \
             API_trace_LOL (req_resp, num, str, f); \
        } while (0)

/**
 * Using the ADSB-LOL API requesting "Route Information" for a call-sign.
 *
 * E.g. sending a request for call-sign `SAS4787` as `https://api.adsb.lol/api/0/route/SAS4787`,
 * should return a JSON-object like this:
 * ```
 *  {
 *   "_airport_codes_iata": "OSL-KEF", << look for this only
 *   "_airports": [                    << Departure airport
 *     {
 *       "alt_feet": 681,
 *       "alt_meters": 207.57,
 *       "countryiso2": "NO",
 *       "iata": "OSL",
 *       "icao": "ENGM",
 *       "lat": 60.193901,
 *       "location": "Oslo",
 *       "lon": 11.1004,
 *       "name": "Oslo Gardermoen Airport"
 *     },
 *     {                               << Destination airport
 *       "alt_feet": 171,
 *       "alt_meters": 52.12,
 *       "countryiso2": "IS",
 *       "iata": "KEF",
 *       "icao": "BIKF",
 *       "lat": 63.985001,
 *       "location": "Reykjavík",
 *       "lon": -22.6056,
 *       "name": "Keflavik International Airport"
 *     }
 *   ],
 *   "airline_code": "SAS",
 *   "airport_codes": "ENGM-BIKF",
 *   "callsign": "SAS4787",
 *   "number": "4787"
 * }
 * ```
 *
 * Similar to `curl.exe -s https://api.adsb.lol/api/0/route/SAS4787 | grep "_airport_codes_iata"`
 *
 * Ref: https://api.adsb.lol/docs#/v0/api_route_api_0_route__callsign__get
 *
 * A response can also contain this (unsupported):
 * ```
 *   "_airport_codes_iata": "BGO-AES-TRD"
 * ```
 *
 * For a departure in "BGO" (Bergen). Stopover in "AES" (Ålesund) and
 * final destination "TRD" (Trondheim).
 */

/**
 * Add an airport record to `g_data.airport_CSV`.
 */
static int CSV_add_entry (const airport *rec)
{
  static airport *copy = NULL;
  static airport *dest = NULL;
  static airport *hi_end;

  if (!g_data.airport_CSV ||   /* Create the initial buffer */
      dest == hi_end - 1)
  {
    size_t new_num = 10000 + g_data.ap_stats.CSV_numbers;

    copy   = realloc (g_data.airport_CSV, sizeof(*rec) * new_num);
    dest   = copy + g_data.ap_stats.CSV_numbers;
    hi_end = copy + new_num;
  }

  /* Causes cvs.c to stop parsing.
   * Hence we now have an incomplete data-set.
   */
  if (!copy)
     return (0);

  g_data.airport_CSV = copy;
  assert (dest < hi_end);
  memcpy (dest, rec, sizeof(*rec));
  g_data.ap_stats.CSV_numbers++;

  if (dest->ICAO[0])
     g_data.ap_stats.CSV_num_ICAO++;

  if (dest->IATA[0])
     g_data.ap_stats.CSV_num_IATA++;

  dest = copy + g_data.ap_stats.CSV_numbers;
  return (1);
}

/**
 * The CSV callback for adding a record to `g_data.airport_CSV`.
 *
 * \param[in]  ctx   the CSV context structure.
 * \param[in]  value the value for this CSV field in record `ctx->rec_num`.
 *
 * Match all 7 fields in a record like this:
 * ```
 * # ICAO, IATA, Full_name, Continent, Location, Longitude, Latitude
 * ENBR,BGO,Bergen Airport Flesland,EU,Bergen,5.21814012,60.29339981
 * ```
 *
 * The `Location, Longitude` fields always uses `.` for decimal-separator.
 * Hence the `"LC_ALL"` locale must be in effect for `strtod()` to work correctly.
 */
static int CSV_callback (struct CSV_context *ctx, const char *value)
{
  static airport rec;
  double         d_val;
  char          *end;
  int            rc = 1;

  if (ctx->field_num == 0)        /* "ICAO" and first field */
  {
    strncpy (rec.ICAO, value, sizeof(rec.ICAO)-1);
  }
  else if (ctx->field_num == 1)   /* "IATA" field */
  {
    strncpy (rec.IATA, value, sizeof(rec.IATA)-1);
  }
  else if (ctx->field_num == 2)   /* "Full_name" field */
  {
    strncpy (rec.full_name, unescape_hex(value), sizeof(rec.full_name)-1);
  }
  else if (ctx->field_num == 3)  /* "Continent" field */
  {
    strncpy (rec.continent, value, sizeof(rec.continent)-1);
  }
  else if (ctx->field_num == 4)  /* "Location" (or City) field */
  {
    strncpy (rec.location, unescape_hex(value), sizeof(rec.location)-1);
  }
  else if (ctx->field_num == 5)  /* "Longitude" field */
  {
    d_val = strtod (value, &end);
    if (end > value)
       rec.pos.lon = d_val;
  }
  else if (ctx->field_num == 6)  /* "Latitude" and last field */
  {
    d_val = strtod (value, &end);
    if (end > value)
       rec.pos.lat = d_val;

    rc = CSV_add_entry (&rec);
    memset (&rec, '\0', sizeof(rec));    /* ready for a new record. */
  }
  return (rc);
}

typedef struct search_stats {
        uint32_t num_lookups;
        uint32_t num_misses;
        double   hit_rate;
      } stats;

static stats g_stats;

#if defined(__MINGW64__) && !defined(HAVE_BSEARCH_S)
static void *bsearch_s (const void *key, const void *base, size_t nmemb, size_t size,
                        int (*compare)(void *, const void *, const void *), void *ctx)
{
  ssize_t min = 0;
  ssize_t max = nmemb - 1;
  ssize_t cursor;
  int     ret;
  void   *p;

  while (min <= max)
  {
    cursor = min + (max - min) / 2;
    p = (char*)base + (cursor * size);
    ret = (*compare) (ctx, key, p);
    if (!ret)
       return (p);

    if (ret < 0)
         max = cursor - 1;
    else min = cursor + 1;
  }
  return (NULL);
}
#endif

/**
 * The compare functions for `qsort_s()` and `bsearch_s()`.
 */
static int CSV_compare_on_ICAO (void *context, const void *_a, const void *_b)
{
  const airport *a = (const airport*) _a;
  const airport *b = (const airport*) _b;
  int    rc = stricmp (a->ICAO, b->ICAO);
  stats *st = (stats*) context;

  st->num_lookups++;
  if (rc)
     st->num_misses++;
  return (rc);
}

static int CSV_compare_on_ICAO_str (const void *a, const void *b)
{
  return stricmp ((const char*)a, (const char*)b);
}

/**
 * Do a binary search for an ICAO airport-name in `g_data.airport_CSV`.
 */
static const airport *CSV_lookup_ICAO (const char *ICAO)
{
  const airport *a = NULL;

  g_stats.num_lookups = 0;
  g_stats.num_misses  = 0;
  g_stats.hit_rate    = 0.0;

  if (g_data.airport_CSV)
  {
    airport key;

    strcpy_s (key.ICAO, sizeof(key.ICAO), ICAO);
    a = bsearch_s (&key, g_data.airport_CSV, g_data.ap_stats.CSV_numbers,
                   sizeof(*g_data.airport_CSV), CSV_compare_on_ICAO, &g_stats);
    if (g_stats.num_lookups)
    {
      if (g_stats.num_misses == 0)
           g_stats.hit_rate = 1.0F;
      else g_stats.hit_rate = 1.0F - ((double)g_stats.num_misses / (double)g_stats.num_lookups);
      g_stats.hit_rate *= 100.0F;
    }
  }
  return (a);
}

/**
 * Do a binary search for an IATA to ICAO airport-name mapping.
 * Does not work.
 */
static const char *IATA_to_ICAO (const char *IATA)
{
  if (!g_data.IATA_to_ICAO || !IATA)
     return (NULL);

  return bsearch (IATA, g_data.IATA_to_ICAO, g_data.ap_stats.CSV_numbers,
                  sizeof(char*), CSV_compare_on_ICAO_str);
}

/**
 * Return a string for an airport-type.
 */
static const char *airport_t_str (airport_t type)
{
  static char buf [20];

  if (type == AIRPORT_CSV)
     return ("CSV");
  if (type == AIRPORT_API_LIVE)
     return ("LIVE");
  if (type == AIRPORT_API_PENDING)
     return ("PENDING");
  if (type == AIRPORT_API_CACHED)
     return ("CACHED");
  if (type == AIRPORT_API_DEAD)
     return ("DEAD");
  if (type == AIRPORT_API_EXPIRED)
     return ("EXPIRED");

  snprintf (buf, sizeof(buf), "%d?", type);
  return (buf);
}

/*
 * Open and parse 'airport-codes.cvs' into the linear list `g_data.airport_CSV`.
 * This list is allocated by `realloc()` and the `g_data.airport` smartlist points
 * it's members into this memory. Hence no need to call `smartlist_del()` for each
 * element at exit.
 *
 * Also create a mapping of "IATA to ICAO" names (`g_data.IATA_to_ICAO`).
 */
static bool airports_init_CSV (void)
{
  double   start_t = get_usec_now();
  uint32_t i, max, num;

  memset (&g_data.csv_ctx, '\0', sizeof(g_data.csv_ctx));
  g_data.csv_ctx.file_name  = Modes.airport_db;
  g_data.csv_ctx.delimiter  = ',';
  g_data.csv_ctx.callback   = CSV_callback;
  g_data.csv_ctx.num_fields = 7;

  if (!Modes.airport_db[0] || !stricmp(Modes.airport_db, "NUL"))
  {
    LOG_STDERR ("Running with no `Modes.airport_db'\n");
    g_data.no_db = true;
    return (true);
  }

  CSV_init_ctx (&g_data.csv_ctx);

  num = CSV_num_fields (&g_data.csv_ctx);
  if (num != g_data.csv_ctx.num_fields)
  {
    LOG_STDERR ("Incorrect number of fields in \"Modes.airport_db = %s\". Got %u, expected %u\n",
                Modes.airport_db, num, g_data.csv_ctx.num_fields);
    return (false);
  }

  if (!CSV_open_and_parse_file(&g_data.csv_ctx))
  {
    LOG_STDERR ("Parsing of \"Modes.airport_db = %s\" failed: %s\n", Modes.airport_db, strerror(errno));
    return (false);
  }

  TRACE ("Parsed %u records in %.3f msec from: \"%s\"\n",
         g_data.ap_stats.CSV_numbers, (get_usec_now() - start_t) / 1E3, true_path(Modes.airport_db));

  TRACE ("ICAO names: %u, IATA names: %u\n",
         g_data.ap_stats.CSV_num_ICAO, g_data.ap_stats.CSV_num_IATA);

  if (g_data.ap_stats.CSV_numbers > 0)
  {
    airport *a = g_data.airport_CSV + 0;

    qsort_s (g_data.airport_CSV, g_data.ap_stats.CSV_numbers, sizeof(*g_data.airport_CSV),
             CSV_compare_on_ICAO, &g_stats);

    for (i = 0; i < g_data.ap_stats.CSV_numbers; i++, a++)
    {
      a->type = AIRPORT_CSV;
      smartlist_add (g_data.airports, a);
    }

    g_data.IATA_to_ICAO = malloc (sizeof(char*) * g_data.ap_stats.CSV_numbers);
    if (g_data.IATA_to_ICAO)
    {
      max = (uint32_t) smartlist_len (g_data.airports);
      for (i = 0; i < max; i++)
      {
        a = smartlist_get (g_data.airports, i);
        g_data.IATA_to_ICAO [i] = a->IATA;
      }
      qsort (g_data.IATA_to_ICAO, g_data.ap_stats.CSV_numbers, sizeof(char*),
             CSV_compare_on_ICAO_str);
    }
  }
  return (true);
}

/*
 * Free memory allocated above.
 */
static void airports_exit_CSV (void)
{
  if (g_data.airport_CSV)
     free (g_data.airport_CSV);

  g_data.airport_CSV = NULL;
  g_data.ap_stats.CSV_numbers = 0;
}

/**
 * \todo
 * Open and parse `Modes.airport_freq_db` into the smartlist `g_data.airport_freq_CSV`
 */
static bool airports_init_freq_CSV (void)
{
  return (true);
}

static void airports_exit_freq_CSV (void)
{
}

#if defined(USE_BIN_FILES)
static int routes_compare (const void *a, const void *b)
{
  return stricmp ((const char*)a, (const char*)b);
}

/**
 * Look in `*route_records` before posting a request to the ADSB-LOL API.
 */
static flight_info *routes_find_by_callsign (const char *call_sign)
{
  static flight_info  f;
  const route_record *r = NULL;
  route_record2       r_copy;
  char                departure [10];
  char                destination [10];
  char                intermediate1 [10];
  char                intermediate2 [10];
  const airport      *dep  = NULL;
  const airport      *dest = NULL;
  const airport      *int1 = NULL;
  const airport      *int2 = NULL;
  int                 rc;

  if (route_records_num > 0)
     r = bsearch (call_sign, route_records, route_records_num,
                  sizeof(route_record), routes_compare);

  if (!r)
     return (NULL);

  memcpy (&r_copy, r, sizeof(r_copy));

  if (sscanf(r_copy.departure, "%4[^-]-%4s", departure, destination) == 2)
  {
    intermediate1 [0] = '\0';
    intermediate2 [0] = '\0';
  }
  else if (sscanf(r_copy.departure, "%4[^-]-%4[^-]-%4s", departure, intermediate1, destination) == 3)
  {
    intermediate2 [0] = '\0';
  }
  else
  {
    rc = sscanf (r_copy.departure, "%4[^-]-%4[^-]-%4[^-]-%4s",
                 departure, intermediate1, intermediate2, destination);
    assert (rc == 4);
  }

  /* Rewrite 'r' into a 'f' record.
   * Convert ICAO names to IATA if possible.
   */
  memset (&f, '\0', sizeof(f));
  strncpy (f.call_sign, call_sign, sizeof(f.call_sign)-1);
  f.type    = AIRPORT_API_CACHED;
  f.created = Modes.start_FILETIME;

  if (departure[0])
     dep = CSV_lookup_ICAO (departure);

  if (destination[0])
     dest = CSV_lookup_ICAO (destination);

  if (intermediate1[0])
     int1 = CSV_lookup_ICAO (intermediate1);

  if (intermediate2[0])
     int2 = CSV_lookup_ICAO (intermediate2);

  strncpy (f.departure,   dep  ? dep->IATA  : r_copy.departure, sizeof(f.departure)-1);
  strncpy (f.destination, dest ? dest->IATA : r_copy.destination, sizeof(f.destination)-1);
//strncpy (f.intermediate1, int1 ? int1 : "?", sizeof(f.intermediate1)-1);
//strncpy (f.intermediate1, int2 ? int2 : "?", sizeof(f.intermediate2)-1);

  MODES_NOTUSED (int1);
  MODES_NOTUSED (int2);
  return (&f);
}
#endif  /* USE_BIN_FILES */

static void routes_find_test (void)
{
#if !defined(USE_BIN_FILES)
  printf ("%s(): '-DUSE_BIN_FILES' not defined.\n", __FUNCTION__);

#else
  size_t num = min (50, route_records_num);
  size_t i, rec_num;

  if (!route_records)
  {
    printf ("\7%s(): 'route_records == NULL!?'.\n"
            "Run 'py -3 ../tools/gen_data.py' again.\n", __FUNCTION__);
    return;
  }

  printf ("%s():\n  Checking %zu random records among %zu records.\n", __FUNCTION__, num, route_records_num);
  printf ("  Record  call-sign  DEP   DEST   Departure            Destination               usec\n"
          "  --------------------------------------------------------------------------------------\n");

  for (i = 0; i < num; i++)
  {
    const flight_info *f;
    const char        *call_sign, *start;
    const char        *dep, *dest;
    double             start_t = get_usec_now();

    rec_num = (size_t) random_range (0, route_records_num - 1);
    call_sign = route_records [rec_num].call_sign;
    start = route_records [rec_num].departure;

    f = routes_find_by_callsign (call_sign);

    /* `f` should never be NULL here
     */
    dep = airport_find_location (f->departure);
    if (!dep)
       dep = "?";

    dest = airport_find_location (f->destination);
    if (!dest)
        dest = "?";

    printf ("  %6zu  %-7s    %.4s  %.4s   %S %S    %7.1lf\n",
            rec_num, call_sign, start, start + 5,
            u8_format(dep, 20),
            u8_format(dest, 20),
            get_usec_now() - start_t);
  }
#endif  /* USE_BIN_FILES */

  puts ("");
}

/**
 * Add a cached flight-info record to `g_data.flight_info`.
 */
static int API_add_entry (const flight_info *rec)
{
  flight_info *f = flight_info_create (rec->call_sign, rec->ICAO_addr, AIRPORT_API_CACHED);

  if (f)
  {
    ULONGLONG ft_diff;

    f->created = rec->created;
    strcpy (f->departure, rec->departure);
    strcpy (f->destination, rec->destination);
    g_data.ap_stats.API_added_CSV++;

    /* Check if 'Modes.start_FILETIME - rec->created > API_MAX_AGE'.
     */
    ft_diff = *(ULONGLONG*) &Modes.start_FILETIME - *(ULONGLONG*) &rec->created;
    if (ft_diff > API_MAX_AGE)
       f->type = AIRPORT_API_EXPIRED;
  }
  return (f ? 1 : 0);
}

static int API_cache_callback (struct CSV_context *ctx, const char *value)
{
  static flight_info rec;
  int    rc = 1;

  if (ctx->field_num == 0)
     rec.type = atoi (value);

  else if (ctx->field_num == 1)
     strncpy (rec.call_sign, value, sizeof(rec.call_sign)-1);

  else if (ctx->field_num == 2)
     strncpy (rec.departure, value, sizeof(rec.departure)-1);

  else if (ctx->field_num == 3)
     strncpy (rec.destination, value, sizeof(rec.destination)-1);

  else if (ctx->field_num == 4)
     rec.ICAO_addr = mg_unhexn (value, 6);

  else if (ctx->field_num == 5)
  {
    char     *end;
    ULONGLONG val = strtoull (value, &end, 10);

    if (end > value)
       rec.created = *(FILETIME*) &val;

    rc = API_add_entry (&rec);
    memset (&rec, '\0', sizeof(rec));    /* ready for a new record. */
  }
  return (rc);
}

static void API_trace (unsigned line, const char *fmt, ...)
{
  char    buf [200], *ptr = buf;
  int     len, left = (int)sizeof(buf);
  va_list args;

  EnterCriticalSection (&Modes.print_mutex);

#if 0
  char *buf2 = NULL;

  modeS_asprintf (&buf2, "%s(%u, %s): ", __FILE__, line,
                  GetCurrentThreadId() == g_data.thread_id ? "API-thread" : "main-thread");

  va_start (args, fmt);
  modeS_vasprintf (&buf2, fmt, args);
  va_end (args);
  modeS_flog (stdout, buf2);
  free (buf2);

#else
  len = snprintf (ptr, left, "%s(%u, %s): ",
                  __FILE__, line, GetCurrentThreadId() == g_data.thread_id ?
                  "API-thread" : "main-thread");
  ptr  += len;
  left -= len;

  va_start (args, fmt);
  vsnprintf (ptr, left, fmt, args);
  va_end (args);
  modeS_flog (stdout, buf);
#endif

  LeaveCriticalSection (&Modes.print_mutex);
}

static void API_trace_LOL (const char *req_resp, uint32_t num, const char *str, const flight_info *f)
{
  char        http_status [20] = "";
  const char *more = "";
  size_t      len = strlen (str);

  EnterCriticalSection (&Modes.print_mutex);

  if (!strcmp(req_resp, "response") && f->http_status > 0)
  {
    snprintf (http_status, sizeof(http_status), "HTTP %d:\n", f->http_status);
    if (f->http_status >= 400 && f->http_status <= 599)   /* limit 40x - 50x responses to 400 bytes */
    {
      if (len > 400)
         more = "\n...";
      len = 400;
    }
  }

  modeS_flogf (Modes.log, "%s # %u (ICAO: %06X): %s", req_resp, num, f->ICAO_addr, http_status);

  /* Do this since `str` could contain `%s` etc.
   */
  fprintf (Modes.log, "%.*s%s", (int)len, str, more);
  fputs ("\n-------------------------------------------------------"
         "---------------------------------------------------------\n",
         Modes.log);

  LeaveCriticalSection (&Modes.print_mutex);
}

/**
 * Return number of PENDING records to resolve.
 */
static int API_num_pending (void)
{
  const flight_info *f;
  int   i, num, max = g_data.flight_info ? smartlist_len (g_data.flight_info) : 0;

  for (i = num = 0; i < max; i++)
  {
    f = smartlist_get (g_data.flight_info, i);
    if (f->type == AIRPORT_API_PENDING)
         num++;
  }
  return (num);
}

/**
 * Dump records of all types.
 */
static void API_dump_records (void)
{
  flight_info_stats  fs;
  const flight_info *f;
  int   i, max;

  puts ("   #  Call-sign  DEP -> DEST  Type    Created (local)           Resp-time (ms)   HTTP\n"
        "  -----------------------------------------------------------------------------------");

  max = smartlist_len (g_data.flight_info);
  for (i = 0; i < max; i++)
  {
    ULONGLONG ft_diff;
    char      dtime [20] = "N/A";
    char      http_status [10] = " - ";

    f = smartlist_get (g_data.flight_info, i);

    if (*(ULONGLONG*)&f->responded)
    {
      ft_diff = *(ULONGLONG*) &f->responded - *(ULONGLONG*) &f->created;
      snprintf (dtime, sizeof(dtime), "%.3lf", (double)ft_diff / 1E4);
    }
    if (f->http_status > 0)
       _itoa (f->http_status, http_status, 10);

    printf ("  %2d: %-8s   %-5s  %-5s %-7s %s  %8s         %s\n",
            i, f->call_sign, f->departure, f->destination,
            airport_t_str(f->type), modeS_FILETIME_to_loc_str(&f->created, true),
            dtime, http_status);
  }
  flight_stats_now (&fs);
  printf ("  Total: %u, pending: %u, live: %u, cached: %u\n\n", fs.total, fs.pending, fs.live, fs.cached);
}

/**
 * Called from show_statistics() to print statistics collected here.
 */
void airports_show_stats (void)
{
  LOG_STDOUT ("! \n");
  LOG_STDOUT ("Airports statistics:\n");
  interactive_clreol();

  LOG_STDOUT ("  %6u CSV records in list.\n", g_data.ap_stats.CSV_numbers);
  interactive_clreol();

  LOG_STDOUT ("  %6u API records in list (%u dead).\n",
              g_data.fs_stats.live + g_data.fs_stats.cached, g_data.fs_stats.dead);
  interactive_clreol();

  LOG_STDOUT ("  %6u API requests sent. %u received (HTTP 404: %u)\n",
              g_data.ap_stats.API_requests_sent,
              g_data.ap_stats.API_response_recv,
              g_data.ap_stats.API_service_404);
  interactive_clreol();

  LOG_STDOUT ("  %6u API records cached. Used %u times.\n",
              g_data.ap_stats.API_added_CSV, g_data.ap_stats.API_used_CSV);
  interactive_clreol();

#if defined(USE_BIN_FILES)
  LOG_STDOUT ("  %6zu Route records. Used %u times.\n",
              route_records_num, g_data.ap_stats.routes_records_used);
  interactive_clreol();
#endif
}

/**
 * Parse the response from `API_thread_func()` and populate `f`.
 *
 * \todo Handle `API_AIRPORT_ICAO` too.
 */
static bool airports_API_parse_response (flight_info *f, char *resp)
{
  flight_info tmp;
  int         num;
  char       *codes;
  bool        rc;

  codes = strstr (resp, API_AIRPORT_IATA);
  if (!codes || !*codes)
     return (false);

  memset (&tmp, '\0', sizeof(tmp));
  codes += strlen (API_AIRPORT_IATA);
  num = sscanf (codes, "\"%30[^-\"]-%30[^-\"]\"", tmp.departure, tmp.destination);

  /* Support a response like this:
   *  "_airport_codes_iata": "LAX-JED-RUH",
   *
   * i.e. final destination == "RUH" (Riyadh).
   */
  if (num != 2 && num != 3)
  {
    g_data.fs_stats.unknown++;
    rc = false;   /* This request becomes a DEAD record */
  }
  else if (!strcmp(codes, "\"unknown\""))
  {
    g_data.fs_stats.unknown++;
    strcpy (f->departure, "?");
    strcpy (f->destination, "?");
    rc = true;
  }
  else
  {
    strcpy (f->departure, tmp.departure);
    strcpy (f->destination, tmp.destination);
    rc = true;
  }
  API_TRACE ("num: %d, tmp.departure: '%s', tmp.destination: '%s'\n",
             num, tmp.departure, tmp.destination);

  return (rc);
}

/**
 * Normalize a call-sign; strip leading zeros in a call-sign.
 *
 * E.g. a request for callsign "SVA037" shall send this request:
 *   https://vrs-standing-data.adsb.lol/routes/SV/SVA37.json
 *
 * Not this:
 *   https://vrs-standing-data.adsb.lol/routes/SV/SVA037.json
 *
 * \ref https://github.com/vradarserver/standing-data/blob/main/routes/schema-01/README.md?plain=1#L18-L26
 */
static bool normalize_callsign (char *normalized, const char *call_sign)
{
  char alpha1 [10];
  char alpha2 [10] = { '\0' };
  int  num = 0;

  if (sscanf(call_sign, "%[A-Z]%d%[A-Z]", alpha1, &num, alpha2) < 2)
  {
    strcpy (normalized, call_sign);
    return (false);
  }
  snprintf (normalized, 10, "%s%d%s", alpha1, num, alpha2);
  return (true);
}

/**
 * This function blocks the `API_thread_func()` function.
 *
 * Send one request at a time for a call-sign to be resolved into
 * a `AIRPORT_API_LIVE` flight-record.
 */
static bool API_thread_worker (flight_info *f)
{
  char *response;
  char  request [200];
  char  normalized [10];
  bool  rc = false;

  /**
   * A route for e.g. callsign "TVS4307" becomes:
   *   https://vrs-standing-data.adsb.lol/routes/TV/TVS4307.json
   *
   * The call-sign in the request must be normalized first.
   */
  f->normalized = normalize_callsign (normalized, f->call_sign);

  snprintf (request, sizeof(request), API_SERVICE_URL, f->call_sign, normalized);

  /* Log complete request to log-file?
   */
  if (g_data.do_trace_LOL)
       API_TRACE_LOL ("request", g_data.ap_stats.API_requests_sent, request, f);
  else API_TRACE ("request # %lu: (ICAO: %06X, was_norm: %d) '%s',\n",
                  g_data.ap_stats.API_requests_sent, f->normalized,
                  f->ICAO_addr, request);

  g_data.ap_stats.API_requests_sent++;

  response = download_to_buf (request);  /* This function blocks */

  get_FILETIME_now (&f->responded);      /* Store time-stamp for ANY response (empty or not) */

  if (!response)
  {
    API_TRACE ("Downloaded no data for %s!\n", f->call_sign);
    return (false);
  }

  g_data.ap_stats.API_response_recv++;

  f->http_status = download_status();

  /* Log complete response to log-file?
   */
  if (g_data.do_trace_LOL)
       API_TRACE_LOL ("response", g_data.ap_stats.API_response_recv - 1, response, f);
  else API_TRACE ("Downloaded %zu bytes data for '%06X/%s': '%.50s'...\n",
                  strlen(response), f->ICAO_addr, f->call_sign, response);

  if (f->http_status == 404)
  {
    g_data.ap_stats.API_service_404++;
  }
  else if (f->http_status == 503 ||     /* We sent too many requests! */
           !strncmp(response, API_SERVICE_503, sizeof(API_SERVICE_503)-1))
  {
    g_data.ap_stats.API_service_503++;
  }
  else
  {
    rc = airports_API_parse_response (f, response);
  }

  free (response);
  return (rc);
}

/**
 * In `--test`, `--debug g` or `--raw` modes (`g_data.do_trace == true`),
 * just trace current flight-stats.
 */
static void airports_API_show_stats (void)
{
  flight_info_stats fs;

  if (g_data.test_mode || (Modes.debug & DEBUG_ADSB_LOL) || Modes.raw)
  {
    flight_stats_now (&fs);
    API_TRACE ("stats now: total=%u, live=%u, pending=%u, dead=%u, unknown=%u\n",
               fs.total, fs.live, fs.pending, fs.dead, fs.unknown);
  }
}

/**
 * The thread for handling flight-info API requests.
 *
 * Iterate over the `g_data.flight_info` list and handle
 * a `AIRPORT_API_PENDING` record one at a time.
 */
static unsigned int __stdcall API_thread_func (void *arg)
{
  airports_priv *data = (airports_priv*) arg;

  while (!Modes.exit)
  {
    if (API_num_pending() == 0) /* Sleep if queue is empty */
    {
      Sleep (API_SLEEP_MS);
      continue;
    }

    int i, max = smartlist_len (g_data.flight_info);
    for (i = 0; i < max; i++)
    {
      flight_info *f = smartlist_get (g_data.flight_info, i);

      if (f->type != AIRPORT_API_PENDING)
         continue;

      /* Change the state to AIRPORT_API_LIVE even
       * for an error-response like `"_airport_codes_iata": "unknown"`
       */
      if (API_thread_worker(f))
      {
        f->type = AIRPORT_API_LIVE;
        data->fs_stats.live++;
        data->fs_stats.pending--;
      }
      else   /* Otherwise it's a dead record */
      {
        f->type = AIRPORT_API_DEAD;
        data->fs_stats.dead++;
      }
    }
  }
  SetEvent (data->thread_event);
  return (0);
}

/**
 * Open for writing or create the `%TEMP%\\dump1090\\ AIRPORT_DATABASE_CACHE` file.
 */
static bool airports_cache_open (FILE **f_ptr)
{
  FILE *f = fopen (Modes.airport_cache, "w+t");
  bool  create = (f_ptr == NULL);

  if (f_ptr)
     *f_ptr = f;

  if (!f)
  {
    LOG_STDERR ("Failed to %s \"%s\": %s\n",
                create ? "create" : "open",
                Modes.airport_cache, strerror(errno));
    return (false);
  }

  fprintf (f, "# type,callsign,departure,destination,icao,timestamp\n");
  if (create)
     fclose (f);
  return (true);
}

static void airports_cache_write (void)
{
  FILE *f = NULL;

  if (g_data.fs_stats.live + g_data.fs_stats.dead > 0)
     airports_cache_open (&f);

  flight_info_exit (f);
  if (f)
  {
    struct stat st;

    fclose (f);
    if (stat(Modes.airport_cache, &st) == 0)
         API_TRACE ("\"%s\": %u bytes written.\n", Modes.airport_cache, st.st_size);
    else API_TRACE ("\"%s\": errno: %d/%s.\n", Modes.airport_cache, errno, strerror(errno));
  }
  if (!f && g_data.test_mode)
     printf ("No need to rewrite the %s cache.\n", Modes.airport_cache);
}

/**
 * Called from `background_tasks()` 4 times per second.
 * Save the `%TEMP%\\dump1090\\AIRPORT_DATABASE_CACHE` file once every 5 minutes.
 *
 * Only effective in `--interactive` mode.
 * \todo make an option / cfg-setting to enable this always.
 */
void airports_background (uint64_t now)
{
  static uint64_t    last_dump = 0;
  static bool        do_dump = true;
  const flight_info *f;
  FILE              *file;
  int                i, max, num;

  airports_API_show_stats();

  if (!do_dump)  /* problem with cache-file? */
     return;

  if ((now - last_dump) < API_CACHE_PERIOD)   /* 5 min not passed */
     return;

  if (last_dump == 0ULL)           /* return if first time called */
  {
    last_dump = now;
    return;
  }

  last_dump = now;

  if (!airports_cache_open(&file))
  {
    do_dump = false;
    return;
  }

  max = smartlist_len (g_data.flight_info);
  for (i = num = 0; i < max; i++)
  {
    f = smartlist_get (g_data.flight_info, i);
    if (flight_info_write(file, f))
       num++;
  }
  fclose (file);

  LOG_FILEONLY2 ("dumped %d LIVE records to cache.\n", num);
}

/**
 * Open and parse the `%TEMP%\\dump1090\\ AIRPORT_DATABASE_CACHE` file
 * and append to `g_data.flight_info`.
 *
 * These records are always `a->type == AIRPORT_API_CACHED`.
 */
static bool airports_init_API (void)
{
  struct stat st;
  bool        exists;

  g_data.thread_event = CreateEvent (NULL, FALSE, FALSE, NULL);
  if (!g_data.thread_event)
  {
    LOG_STDERR ("Failed to create thread-event: %lu\n", GetLastError());
    return (false);
  }

  if (Modes.https_lol_API)
  {
     /**< \todo */
  }
  else
  {
    g_data.thread_hnd = _beginthreadex (NULL, 0, API_thread_func, &g_data, 0, &g_data.thread_id);
    if (!g_data.thread_hnd)
    {
      LOG_STDERR ("Failed to create thread: %s\n", strerror(errno));
      return (false);
    }
  }

  exists = (stat(Modes.airport_cache, &st) == 0 && st.st_size > 0);
  if (!exists)
     airports_cache_open (NULL);
  else
  {
    flight_info_stats fs;

    memset (&g_data.csv_ctx, '\0', sizeof(g_data.csv_ctx));
    g_data.csv_ctx.file_name  = Modes.airport_cache;
    g_data.csv_ctx.delimiter  = ',';
    g_data.csv_ctx.callback   = API_cache_callback;
    g_data.csv_ctx.line_size  = 2000;
    g_data.csv_ctx.num_fields = 6;

    if (!CSV_open_and_parse_file(&g_data.csv_ctx))
    {
      API_TRACE ("c_in: 0x%02X, state: %d\n", g_data.csv_ctx.c_in, g_data.csv_ctx.state);

      /* getting only the header from a previous run is not an error
       */
      if (g_data.csv_ctx.state != STATE_EOF &&
          g_data.csv_ctx.state != STATE_NORMAL)
      {
        LOG_STDERR ("Parsing of \"Modes.airport_cache = %s\" failed: %s\n", Modes.airport_cache, strerror(errno));
        return (false);
      }
    }

    flight_stats_now (&fs);
    TRACE ("Parsed %u/%u/%u records from: \"%s\"\n",
           fs.cached, fs.expired, g_data.ap_stats.API_added_CSV, Modes.airport_cache);

    if (g_data.test_mode)
       assert (fs.cached + fs.expired == g_data.ap_stats.API_added_CSV);
  }
  return (true);
}

/**
 * Return ret-val from `airports_init()`
 * and number of airports
 */
uint32_t airports_num (uint32_t *num)
{
  *num = g_data.ap_stats.CSV_numbers;
  assert (g_data.init_done);
  return (g_data.last_rc);
}

#if defined(USE_BIN_FILES)
static void *read_route_records (FILE *f, const BIN_header *hdr)
{
  size_t size = hdr->rec_len * hdr->rec_num;
  void  *mem = malloc (size);

  if (!mem)
  {
    LOG_STDERR ("Failed to allocate %zu bytes for %s!\n", size, g_data.routes_bin);
    return (NULL);
  }
  if (fread(mem, 1, size, f) != size)
  {
    LOG_STDERR ("Failed to read %zu bytes for %s!\n", size, g_data.routes_bin);
    return (NULL);
  }
  route_records_num = hdr->rec_num;
  return (mem);
}

/**
 * Check a single .BIN database for existance and age.is not older than 10 days.
 * \retval false  no update needed.
 * \retval true   update needed.
 */
static bool airports_update_BIN_file (const char *fname)
{
  struct stat st;
  bool        force_it = false;

  if (stat(fname, &st) != 0)
  {
    LOG_STDERR ("\nForce updating '%s' since it does not exist.\n", fname);
    force_it = true;
  }
  else
  {
    time_t when, now = time (NULL);
    time_t expiry = now - 10 * 24 * 3600;

    if (st.st_mtime > expiry)
    {
      when = now + 10 * 24 * 3600;  /* 10 days into the future */
      if (Modes.update)
         LOG_STDERR ("\nUpdate of '%s' not needed before %.24s.\n", fname, ctime(&when));
    }
    else
      force_it = true;
  }
  return (force_it);
}

/**
 * Check if the `airports.bin`, `aircraft.bin` or `routes.bin` databases
 * needs update. If so, run `py.exe -3 ../tools/gen_data.py` to update all of them.
 */
bool airports_update_BIN (void)
{
  int  need_update = 0;
  bool rc = true;

  if (airports_update_BIN_file(g_data.airports_bin))
     need_update++;

  if (airports_update_BIN_file(g_data.aircrafts_bin))
     need_update++;

  if (airports_update_BIN_file(g_data.routes_bin))
     need_update++;

  if (need_update > 0)
  {
    char cmd [200];

    snprintf (cmd, sizeof(cmd), "py.exe -3 %s\\tools\\gen_data.py", Modes.where_am_I);

    LOG_STDERR ("Updating .BIN-files using '%s'\n\n", cmd);
    rc = (system (cmd) == 0);
  }
  return (rc);
}

static int airports_set_BIN_file (mg_file_path *file, const char *bin_file)
{
  struct stat st;
  bool   exist, truncated = false;

  snprintf (*file, sizeof(*file), "%s\\%s", Modes.results_dir, bin_file);
  exist = (stat(*file, &st) == 0);

  if (exist)
     truncated = (st.st_size < sizeof(BIN_header));

  if (!exist)
     LOG_STDERR ("file: '%s' is missing.\n", *file);
  if (truncated)
     LOG_STDERR ("file: '%s' is truncated.\n", *file);

  return (exist && !truncated);
}

static FILE *airports_init_one_BIN (const char *fname, BIN_header *hdr)
{
  FILE  *f = fopen (fname, "rb");
  time_t created;

  if (!f)
  {
    LOG_STDERR ("Failed to open %s! errno: %d/%s\n", fname, errno, strerror(errno));
    return (NULL);
  }
  if (fread(hdr, 1, sizeof(*hdr), f) != sizeof(*hdr))
  {
    LOG_STDERR ("Failed to read header for %s!\n", fname);
    fclose (f);
    return (NULL);
  }

  created = hdr->created;
  TRACE ("bin_marker: %.*s\n",  (int)sizeof(hdr->bin_marker), hdr->bin_marker);
  TRACE ("created:    %.24s\n", ctime(&created));
  TRACE ("rec_len:    %u\n",    hdr->rec_len);
  TRACE ("rec_num:    %u\n\n",  hdr->rec_num);
  return (f);
}

static bool airports_init_BIN (void)
{
  BIN_header hdr;
  FILE      *f;

  if (airports_set_BIN_file (&g_data.aircrafts_bin, "aircraft.bin") +
      airports_set_BIN_file (&g_data.airports_bin, "airports.bin")  +
      airports_set_BIN_file (&g_data.routes_bin, "routes.bin") == 0)
     return (false);

  f = airports_init_one_BIN (g_data.routes_bin, &hdr);
  if (!f)
     return (false);

  route_records = read_route_records (f, &hdr);
  route_records_num = hdr.rec_num;
  fclose (f);

  f = airports_init_one_BIN (g_data.aircrafts_bin, &hdr);
  // ...
  if (f)
     fclose (f);

  f = airports_init_one_BIN (g_data.airports_bin, &hdr);
  // ...
  if (f)
     fclose (f);

  return (route_records ? true : false);
}

#else
static bool airports_init_BIN (void)
{
  return (true);
}
#endif

/**
 * Main init function for this module.
 */
uint32_t airports_init (void)
{
  bool rc;

  assert (g_data.airports == NULL);
  assert (g_data.init_done == false);

  g_data.flight_info = smartlist_new();
  g_data.airports    = smartlist_new();
  g_data.test_mode   = test_contains (Modes.tests, "airport");

  if (!g_data.flight_info || !g_data.airports)
  {
    LOG_STDERR ("No memory for smartlists\n");
    rc = false;
    goto done;
  }

  if (Modes.debug & DEBUG_ADSB_LOL)
     g_data.do_trace_LOL = true;
  else if ((Modes.debug & DEBUG_GENERAL) || Modes.tests)
     g_data.do_trace = true;

  Modes.airports_priv = &g_data;

  rc = (airports_init_BIN() &&
        airports_init_CSV() &&
        airports_init_freq_CSV() &&
        airports_init_API());

#if defined(USE_BIN_FILES)
  if (Modes.update)
  {
    airports_update_BIN();
    rc = false;
    goto done;
  }
#endif

  if (g_data.test_mode)
  {
    SetConsoleOutputCP (CP_UTF8);

    patch_call_signs_tests();
    airport_CSV_test_1();
    airport_CSV_test_2();
    airport_CSV_test_3();
    airport_CSV_test_4();
    airport_loc_test_1();
    airport_loc_test_2();
    airport_API_test_1();
    airport_API_test_2();
    airport_normalize_test();
    routes_find_test();
    airports_show_stats();
  }

  if (test_contains(Modes.tests, "locale"))
     locale_test();

  /*
   * On success, return the number of airport CSV records.
   * it's needed in `interactive_init()` before deciding
   * to show the "DEP DST" columns.
   *
   * If we have 0 airports, it's little point showing these columns
   * allthough the ADSB-LOL API does not need that information.
   */
done:
  g_data.init_done = true;
  g_data.last_rc = rc ? (g_data.ap_stats.CSV_numbers > 0 || g_data.no_db) : 0;
  return (g_data.last_rc);
}

/**
 * Free the resources used by the ADSB-LOL API.
 */
static void airports_exit_API (void)
{
  smartlist_free (g_data.airports);
  g_data.airports = NULL;

  free (g_data.IATA_to_ICAO);
  g_data.IATA_to_ICAO = NULL;

  if (g_data.thread_hnd)
  {
    WaitForSingleObject (g_data.thread_event, 500);
    CloseHandle (g_data.thread_event);
    CloseHandle ((HANDLE)g_data.thread_hnd);
  }
  g_data.thread_hnd = 0ULL;
}

void airports_exit (bool free_airports)
{
  if (free_airports)  /* If a SIGABRT was NOT raised, exit normally */
  {
    airports_exit_API();
    airports_exit_CSV();
    airports_exit_freq_CSV();
  }

  /* Otherwise at least try to save the cache
   * if the list is non-empty.
   */
  if (g_data.flight_info)
  {
    if (smartlist_len(g_data.flight_info) > 0)
       airports_cache_write();
    smartlist_free (g_data.flight_info);
  }

  g_data.flight_info = NULL;
  Modes.airports_priv = NULL;

#if defined(USE_BIN_FILES)
  free (route_records);
  route_records_num = 0;
#endif
}

/**
 * Find the airport location by it's IATA name.
 * Currently must tranverse the whole list :-(
 */
const char *airport_find_location_by_IATA (const char *IATA)
{
  int i, max;

  if (!IATA[0])
     return (NULL);

  max = smartlist_len (g_data.airports);
  for (i = 0; i < max; i++)
  {
    const airport *a = smartlist_get (g_data.airports, i);

    if (!stricmp(IATA, a->IATA))
       return (a->location[0] ? a->location : NULL);
  }
  return (NULL);
}

const char *airport_find_location_by_ICAO (const char *ICAO)
{
  int i, max;

  if (!ICAO[0])
     return (NULL);

  max = smartlist_len (g_data.airports);
  for (i = 0; i < max; i++)
  {
    const airport *a = smartlist_get (g_data.airports, i);

    if (!stricmp(ICAO, a->ICAO))
       return (a->location[0] ? a->location : NULL);
  }
  return (NULL);
}

/**
 * Find the airport location by either IATA or ICAO name.
 */
const char *airport_find_location (const char *IATA_or_ICAO)
{
  if (!IATA_or_ICAO[0])
     return (NULL);

  int i, max = smartlist_len (g_data.airports);
  for (i = 0; i < max; i++)
  {
    const airport *a = smartlist_get (g_data.airports, i);

    if (!stricmp(IATA_or_ICAO, a->ICAO) || !stricmp(IATA_or_ICAO, a->IATA))
       return (a->location[0] ? a->location : NULL);
  }
  return (NULL);
}

/**
 * Dumping of airport data:
 *  \li print a starting header.
 *  \li print the actual record for an `airport*`
 */
static void airport_print_header (unsigned line, bool use_usec)
{
  usec_fmt = (use_usec ? "%.2f" : "%.2f%%");

  printf ("line: %u:\n"
          "  Record  ICAO       IATA       cont location               full_name                                            lat       lon  %s\n"
          "  --------------------------------------------------------------------------------------------------------------------------------------\n",
          line, use_usec ? "usec" : "hit-rate");
}

static void airport_print_rec (const airport *a, const char *ICAO, size_t idx, double val, bool UTF8_decode)
{
  const pos_t  pos0 = { 0.0, 0.0 };
  char         val_buf [20] = { "-" };  /* usec or hit-rate */
  const char  *IATA             = a ? a->IATA      : "?";
  const char  *full_name        = a ? a->full_name : "?";
  const char  *continent        = a ? a->continent : "?";
  const char  *location         = a ? a->location  : "?";
  const pos_t *pos              = a ? &a->pos      : &pos0;

  printf ("  %5zu   '%-8.8s' '%-8.8s' %2.2s   ", idx, ICAO, IATA, continent);

  if (val >= 0.0)
     snprintf (val_buf, sizeof(val_buf), usec_fmt, val);

  if (UTF8_decode)
       printf ("'%S' '%S' ", u8_format(location, 20), u8_format(full_name, 42));
  else printf ("'%-20.20s' '%-43.43s'", location, full_name);

  printf ("  %9.3f %9.3f  %s\n", pos->lat, pos->lon, val_buf);
}

/**
 * Do some simple tests on the `g_data.airport_CSV`.
 */
static void airport_CSV_test_1 (void)
{
  const  airport *a;
  size_t i, i_max = 10;

  printf ("%s():\n  Dumping %zu airport records: ", __FUNCTION__, i_max);
  AIRPORT_PRINT_HEADER (false);

  size_t max = (size_t) smartlist_len (g_data.airports);
  for (i = 0; i < max && i < i_max; i++)
  {
    a = smartlist_get (g_data.airports, i);
    airport_print_rec (a, a->ICAO, i, -1.0F, false);
  }
  puts ("");
}

#define ADD_AIRPORT(ICAO, IATA, continent, location, \
                    full_name, lon, lat)             \
                 { { ICAO },                         \
                   { IATA },                         \
                   { continent },                    \
                   { location  },                    \
                   { full_name },                    \
                   { lon, lat  },                    \
                   AIRPORT_CSV                       \
                 }

static const airport airport_tests [] = {
       ADD_AIRPORT ("ENBR",    "BGO", "EU", "Bergen",               "Bergen Airport Flesland",                5.218140120, 60.2933998100),
       ADD_AIRPORT ("DE-0036", "?",   "EU", "Grafenw\xC3\xB6hr",    "Grafenw\xC3\xB6hr Medevac Helipad",     11.916111111, 49.7080555556),
       ADD_AIRPORT ("ENGM",    "OSL", "EU", "Oslo",                 "Oslo Gardermoen Airport",               11.100399971, 60.193901062012),
       ADD_AIRPORT ("KJFK",    "JFK", "NA", "New York",             "John F Kennedy International Airport", -73.778000000, 40.6398010000),
       ADD_AIRPORT ("OTHH",    "DOH", "AS", "Doha",                 "Hamad International Airport",           51.608050000, 25.2730560000),
       ADD_AIRPORT ("XXXX",    "XXX", "XX", "----",                 "---",                                    0.0,         0.0),           /* test for low hit_rate */
       ADD_AIRPORT ("AF10",    "URZ", "AS", "Or\xC5\xABg\xC4\x81n", "Or\xC5\xABg\xC4\x81n Airport",          66.630897520, 32.9029998779)  /* Uruzgan / Afghanistan */
     };

static void airport_CSV_test_2 (void)
{
  const airport *t = airport_tests + 0;
  size_t         i, num_ok;

  printf ("%s():\n  Checking %zu fixed records. ", __FUNCTION__, DIM(airport_tests));
  AIRPORT_PRINT_HEADER (false);

  for (i = num_ok = 0; i < DIM(airport_tests); i++, t++)
  {
    const airport *a = CSV_lookup_ICAO (t->ICAO);

    if (a && !memcmp(a->location, t->location, sizeof(t->location)))
       num_ok++;

    if (!a && !strcmp(t->ICAO, "XXXX"))   /* the "XXXX" will never be found. That's OK */
       num_ok++;

    airport_print_rec (a, t->ICAO, i, g_stats.hit_rate, true);
  }
  printf ("  %3zu OKAY\n", num_ok);
  printf ("  %3zu FAIL\n\n", i - num_ok);
}

static void airport_CSV_test_3 (void)
{
  size_t i, num = min (10, g_data.ap_stats.CSV_numbers);

  printf ("%s():\n  Checking %zu random records. ", __FUNCTION__, num);
  AIRPORT_PRINT_HEADER (true);

  for (i = 0; i < num; i++)
  {
    double         now     = get_usec_now();
    size_t         rec_num = (size_t) random_range (0, g_data.ap_stats.CSV_numbers - 1);
    const char    *ICAO    = g_data.airport_CSV [rec_num].ICAO;
    const airport *a       = CSV_lookup_ICAO (ICAO);

    airport_print_rec (a, ICAO, rec_num, get_usec_now() - now, true);
  }
  puts ("");
}

static void airport_CSV_test_4 (void)
{
  size_t i, num = min (10, g_data.ap_stats.CSV_numbers);

  printf ("%s():\n  Checking %zu random records.\n", __FUNCTION__, num);
  puts ("    Rec   ICAO      ICAO2       full_name\n"
        "  -------------------------------------------------------------------");

  for (i = 0; i < num; i++)
  {
    const airport *a;
    const char    *ICAO, *ICAO2;
    unsigned       rec_num = random_range (0, g_data.ap_stats.CSV_numbers - 1);

    ICAO = g_data.airport_CSV [rec_num].ICAO;
    a    = CSV_lookup_ICAO (ICAO);
    if (a && a->IATA[0])
         ICAO2 = IATA_to_ICAO (a->IATA);
    else ICAO2 = NULL;
    printf ("  %5u  '%-8s' '%-8s' '%s'\n",
            rec_num, ICAO, ICAO2 ? ICAO2 : "?",
            a ? a->full_name : "?");
  }
  puts ("");
}

/**
 * Print header and line for a location test.
 */
#define LOCATION_PRINT_HEADER() \
        puts ("  Record   ICAO        IATA    cont  location                full_name\n" \
              "  -------------------------------------------------------------------" \
              "--------------------------------")

static void location_print_rec (unsigned idx, const char *ICAO, const char *IATA, const char *cont,
                                const char *location, const char *full_name)
{
  if (!location)
      location = "?";
  if (!full_name)
      full_name = "?";

  printf ("  %5u    '%-8s'  '%-3s'   %2s    '%S'  %S\n",
          idx,
          ICAO[0]   ? ICAO : "?",
          IATA[0]   ? IATA : "?",
          cont[0]   ? cont : "?",
          u8_format(location, 20), u8_format(full_name, 0));
}

/**
 * Test finding airport location names by IATA names.
 */
static void airport_loc_test_1 (void)
{
  const airport *t = airport_tests + 0;
  unsigned       i;

  printf ("%s():\n  Checking %zu fixed records.\n", __FUNCTION__, DIM(airport_tests));
  LOCATION_PRINT_HEADER();

  for (i = 0; i < DIM(airport_tests); i++, t++)
  {
    const char *location = airport_find_location_by_IATA (t->IATA);

    location_print_rec (i, t->ICAO, t->IATA, t->continent, location, NULL);
  }
  puts ("");
}

/**
 * Test finding airport location names by ICAO / IATA names randomly.
 */
static void airport_loc_test_2 (void)
{
  size_t i, num = min (20, g_data.ap_stats.CSV_numbers);

  printf ("%s():\n  Checking %zu random records.\n", __FUNCTION__, num);

  LOCATION_PRINT_HEADER();

  for (i = 0; i < num; i++)
  {
    const char *ICAO, *IATA, *cont, *location;
    unsigned    rec_num = (size_t) random_range (0, g_data.ap_stats.CSV_numbers - 1);

    IATA = g_data.airport_CSV [rec_num].IATA;
    ICAO = g_data.airport_CSV [rec_num].ICAO;
    cont = g_data.airport_CSV [rec_num].continent;

    location = airport_find_location_by_ICAO (ICAO);
    if (!location)
       location = airport_find_location_by_IATA (IATA);

    location_print_rec (rec_num, ICAO, IATA, cont, location, g_data.airport_CSV[rec_num].full_name);
  }
  puts ("");
}

/**
 * Test `airport_normalize_call_sign()`.
 *
 * https://github.com/vradarserver/standing-data/blob/main/routes/schema-01/README.md?plain=1#L54
 */
typedef struct normalize_test {
        char before [10];
        char after  [10];
      } normalize_test;

static void airport_normalize_test (void)
{
  static const normalize_test tests[] = {
            { "EZY1200", "EZY1200" },
            { "EZY0001", "EZY1"    },
            { "EZY0000", "EZY0"    },
            { "EZY00AB", "EZY0AB"  },
            { "SVA037",  "SVA37"   },
            { "U21234",  "EZY1234" }  /* IATA code "U2" is ICAO code "EZY". Not implemented */
          };
  const normalize_test *t = tests + 0;

  printf ("%s():\n", __FUNCTION__);

  for (size_t i = 0; i < DIM(tests); i++, t++)
  {
    char norm [10];
    bool okay;

    normalize_callsign (norm, t->before);
    okay = (strcmp (norm, tests[i].after) == 0);
    printf ("  %-8s -> %-8s  %s", tests [i].before, norm, okay ? "OK" : "FAIL");
    if (!okay)
       printf (", should be %s", tests[i].after);
    puts ("");
  }
  puts ("");
}

static void patch_call_signs_tests (void)
{
#if defined(USE_BIN_FILES)
  size_t i, num;

  printf ("%s():\n", __FUNCTION__);

  for (i = 0; i < DIM(call_signs_tests); i++)
  {
    num = (size_t) random_range (0, route_records_num - 1);
    call_signs_tests [i] = route_records [num].call_sign;
  }
#endif
}

/**
 * Test the ADSB-LOL API lookup.
 */
static void airport_API_test_2 (void)
{
  int  i, save = g_data.do_trace;

  g_data.do_trace = false;
  printf ("%s():\n  Testing IATA to ICAO lookups.\n", __FUNCTION__);

  for (i = 0; i < (int)DIM(call_signs_tests); i++)
  {
    const char    *IATA_departure, *IATA_destination;
    const char    *ICAO_departure, *ICAO_destination;
    const airport *ICAO_airport;

    bool rc = airports_API_get_flight_info (call_signs_tests[i], ICAO_UNKNOWN,
                                            &IATA_departure, &IATA_destination);
    if (!rc)
       continue;

    ICAO_departure   = IATA_to_ICAO (IATA_departure);
    ICAO_destination = IATA_to_ICAO (IATA_destination);

    if (ICAO_departure)
    {
      ICAO_airport = CSV_lookup_ICAO (ICAO_departure);
      if (ICAO_airport)
         ICAO_departure = ICAO_airport->ICAO;
    }
    if (ICAO_destination)
    {
      ICAO_airport = CSV_lookup_ICAO (ICAO_destination);
      if (ICAO_airport)
         ICAO_destination = ICAO_airport->ICAO;
    }

    printf ("    %-8s: %-6s -> %-6s (%s - %s)\n",
            call_signs_tests[i], IATA_departure, IATA_destination,
            ICAO_departure   ? ICAO_departure   : "?",
            ICAO_destination ? ICAO_destination : "?");
  }
  g_data.do_trace = save;
  puts ("");
}

/**
 * Do a simple test on some call-signs using the ADSB-LOL API
 * setup in `airports_init_API()`.
 */
static void airport_API_test_1 (void)
{
  flight_info_stats fs;
  uint32_t    t_max, t_elapsed;
  int         i, num_pending;
  bool        save = g_data.do_trace;
  bool        pending_complete = false;
  const char *departure, *destination;

  printf ("%s(), lookup phase (Num-cached: %d):\n", __FUNCTION__, g_data.fs_stats.cached);

  g_data.do_trace = false;   /* do not let API_thread_worker() disturb us */
  t_elapsed = 0;
  t_max = 1000;

  for (i = 0; i < (int)DIM(call_signs_tests); i++)
  {
    airports_API_get_flight_info (call_signs_tests[i], ICAO_UNKNOWN, &departure, &destination);
    t_max += 200;  /* assume a slow internet connection */
  }

  if (Modes.under_appveyor)
     t_max += 2000;  /* Even slower on AppVeyor */

  API_dump_records();
  num_pending = API_num_pending();

  while (num_pending)
  {
    if (API_num_pending() == 0) /* still PENDING records? */
    {
      pending_complete = true;
      break;
    }
    printf ("  Waiting for `API_thread_worker()` to complete (%u ms).\n", t_elapsed);
    Sleep (API_SLEEP_MS);
    t_elapsed += API_SLEEP_MS;
    if (t_elapsed >= t_max || Modes.exit)
       break;
  }

  flight_stats_now (&fs);
  printf ("  t_elapsed: %u, t_max: %u, Num-live: %u, Num-pending: %u Num-cached: %u.\n",
          t_elapsed, t_max, fs.live, fs.pending, g_data.ap_stats.API_added_CSV);

  if (num_pending > 0)   /* if there WAS any PENDING records */
  {
    if (!pending_complete)
         puts ("\n  Results now (incomplete):");
    else puts ("\n  Results now:");
    API_dump_records();
  }
  else
    puts ("  No PENDING records to wait for.\n");

  g_data.do_trace = save;
}

static void locale_test (void)
{
  const char *str1 = "1024.123456789";
  const char *str2 = "1024,123456789";
  char      **end = NULL;
  const char *_l, *l;
  const char *locales[] = { "en", "nb_NO", "de_DE", "C" };
  const struct lconv *loc;
  size_t      i;

  _configthreadlocale (_ENABLE_PER_THREAD_LOCALE);
  loc = localeconv();

  printf ("\n%s():\n", __FUNCTION__);

  printf ("  int_curr_symbol:   '%s'\n", loc->int_curr_symbol);
  printf ("  decimal_point:     '%s'\n", loc->decimal_point);
  printf ("  mon_decimal_point: '%s'\n", loc->mon_decimal_point);

  printf ("  str1 = \"%s\"\n", str1);
  printf ("  str2 = \"%s\"\n", str2);

  for (i = 0, l = locales[0]; i < DIM(locales); l++, i++)
  {
    _l = setlocale (LC_NUMERIC, locales[i]);
    if (!_l)
        _l = "?";
    printf ("  %-5s: str1 -> %.15g\n", _l, strtod(str1, end));
    printf ("  %-5s: str2 -> %.15g\n", _l, strtod(str2, end));
  }
  puts ("");
}

/**
 * Handling of "Flight Information".
 *
 * Created a smartlist of pending, live or cached flight-information.
 */
static flight_info *flight_info_create (const char *call_sign, uint32_t addr, airport_t type)
{
  flight_info *f = calloc (sizeof(*f), 1);

  if (!f)
     return (NULL);

  f->type      = type;
  f->ICAO_addr = addr;
  get_FILETIME_now (&f->created);
  strncpy (f->call_sign, call_sign, sizeof(f->call_sign)-1);
  strcpy (f->departure, "?");
  strcpy (f->destination, "?");

  smartlist_add (g_data.flight_info, f);

  g_data.fs_stats.total++;
  switch (type)
  {
    case AIRPORT_API_PENDING:
         g_data.fs_stats.pending++;
         break;
    case AIRPORT_API_CACHED:
         g_data.fs_stats.cached++;
         break;
    case AIRPORT_API_EXPIRED:
         g_data.fs_stats.expired++;
         break;
    default:
         assert (0);
         break;
  }
  return (f);
}

/**
 * As above, but for any ICAO address (or a "live" ICAO address).
 */
static flight_info *flight_info_find_by_addr (uint32_t addr)
{
  int i, max = smartlist_len (g_data.flight_info);

  for (i = 0; i < max; i++)
  {
    flight_info *f = smartlist_get (g_data.flight_info, i);

    if (f->ICAO_addr != ICAO_UNKNOWN && addr == f->ICAO_addr)
       return (f);
  }
  return (NULL);
}

/**
 * Find `flight_info*` for a `call_sign` in either
 * `route_records*` or the `g_data.flight_info` cache.
 *
 * If `Modes.prefer_ADSB_LOL == true` (from the config-file), search in
 * `g_data.flight_info` cache. If not found there, we return NULL to create
 * a new `AIRPORT_API_PENDING` record handled by 'API_thread_worker()'.
 */
static flight_info *find_by_callsign (const char *call_sign, bool *fixed)
{
  flight_info *f;
  int    i, max;

  *fixed = false;

#if defined(USE_BIN_FILES)
  if (!Modes.prefer_adsb_lol)
  {
    f = routes_find_by_callsign (call_sign);
    if (f)
    {
      *fixed = true;
      return (f);
    }
  }
#endif

  max = smartlist_len (g_data.flight_info);
  for (i = 0; i < max; i++)
  {
    f = smartlist_get (g_data.flight_info, i);
    if (!stricmp(f->call_sign, call_sign))
       return (f);
  }
  return (NULL);
}

/**
 * Write a `g_data.flight_info` element to file-cache.
 */
static bool flight_info_write (FILE *file, const flight_info *f)
{
  const char *departure   = f->departure;
  const char *destination = f->destination;

  if (!stricmp(f->departure, "unknown"))
     departure = "?";
  if (!stricmp(f->destination, "unknown"))
     destination = "?";

  if (*departure == '?' || *destination == '?')
     return (false);

  /* Cache only LIVE / CACHED records
   */
  if (f->type != AIRPORT_API_LIVE && f->type != AIRPORT_API_CACHED)
     return (false);

  fprintf (file, "%d,%s,%s,%s,%06X,%llu\n",
           f->type, f->call_sign, departure, destination, f->ICAO_addr,
           *(const ULONGLONG*) &f->created);
  return (true);
}

/**
 * Exit function for flight-info:
 *  \li Write the `AIRPORT_API_CACHED` or `AIRPORT_API_LIVE` records to `Modes.airport_cache`.
 *  \li Free the `g_data.flight_info` list.
 */
static void flight_info_exit (FILE *file)
{
  int i, max = smartlist_len (g_data.flight_info);

  for (i = 0; i < max; i++)
  {
    flight_info *f = smartlist_get (g_data.flight_info, i);

    if (file)
       flight_info_write (file, f);
    free (f);
  }
}

/**
 * Return counters for flight-info active now.
 */
static void flight_stats_now (flight_info_stats *fs)
{
  int i, max = smartlist_len (g_data.flight_info);

  memset (fs, '\0', sizeof(*fs));

  for (i = 0; i < max; i++)
  {
    const flight_info *f = smartlist_get (g_data.flight_info, i);

    fs->total++;
    switch (f->type)
    {
      case AIRPORT_API_LIVE:
           fs->live++;
           break;
      case AIRPORT_API_PENDING:
           fs->pending++;
           break;
      case AIRPORT_API_CACHED:
           fs->cached++;
           break;
      case AIRPORT_API_DEAD:
           fs->dead++;
           break;
      case AIRPORT_API_EXPIRED:
           fs->expired++;
           break;
      default:
           TRACE ("record %u: Unknown f->type: %d\n", fs->total, f->type);
           break;
    }
  }
}

/**
 * Non-blocking function called to get flight-information for a single call-sign.
 *
 * Add to the API lookup-queue if not already in the `g_data.flight_info` smartlist.
 * \param call_sign   [in]      the call-sign to resolve.
 * \param addr        [in]      the plane's ICAO address.
 * \param departure   [in|out]  a pointer to the IATA departure airport.
 * \param destination [in|out]  a pointer to the IATA destianation airport.
 */
bool airports_API_get_flight_info (const char  *call_sign,
                                   uint32_t     addr,
                                   const char **departure,
                                   const char **destination)
{
  const char  *type;
  const char  *end;
  bool         fixed;
  bool         log_it = !(Modes.debug & DEBUG_PLANE);  /* Do not disturb */
  bool         save = g_data.do_trace;
  flight_info *f;

  *departure = *destination = NULL;

  if (!log_it)
     g_data.do_trace = false;

  /* No point getting the route-info for a helicopter.
   * There will never be any response for such a request.
   */
  if (aircraft_is_helicopter(addr, NULL))
  {
    API_TRACE ("addr: %06X is a helicoper\n", addr);
    g_data.do_trace = save;
    return (false);
  }

  if (*call_sign == '\0')
  {
    API_TRACE ("Empty 'call_sign'!\n");
    g_data.do_trace = save;
    return (false);
  }

  end = strrchr (call_sign, '\0');
  assert (end[-1] != ' ');

  f = find_by_callsign (call_sign, &fixed);
  if (!f)
  {
    flight_info_create (call_sign, addr, AIRPORT_API_PENDING);
    API_TRACE ("Created PENDING record for call_sign: '%s'\n", call_sign);
    g_data.do_trace = save;
    return (false);
  }

  if (f->type == AIRPORT_API_CACHED)
  {
    if (fixed)
         g_data.ap_stats.routes_records_used++;
    else g_data.ap_stats.API_used_CSV++;
  }

  type = airport_t_str (f->type);

  if (f->type == AIRPORT_API_LIVE || f->type == AIRPORT_API_CACHED)
  {
    if (!f->done_trace)
       API_TRACE ("call_sign: '%s', type: %s, '%s' -> '%s', f: 0x%p, fixed: %d\n",
                  call_sign, type, f->departure, f->destination, f, fixed);

    f->done_trace = true;
    *departure    = f->departure;
    *destination  = f->destination;
    g_data.do_trace = save;
    return (true);
  }

  if (!f->done_trace)
     API_TRACE ("call_sign: '%s', type: %s, ? -> ?\n", call_sign, type);

  f->done_trace   = true;
  g_data.do_trace = save;
  return (false);
}

/*
 * The 3 functions below are called from `interactive_show_aircraft()`
 * and `--interactive` mode only.
 *
 * Log a plane entering and after some time (when we have the call-sign),
 * log the resolved flight-info from the 'ADSB-LOL API' for this plane's
 * call-sign and ICAO-address. Like:
 *   ```
 *   20:08:34.123: plane 48AE23 entering.
 *   20:08:36.456: plane 48AE23, call-sign: SK295, "WAW -> JFK" (Warsaw -> New York, 120.34 msec)
 *   ....
 *   20:14:32.789: plane 48AE23 leaving. Active for 356.4 sec...
 *   ```
 */
bool airports_API_flight_log_entering (const aircraft *a)
{
  LOG_FILEONLY ("%s %06X entering%s\n", a->is_helicopter ? "helicopter" : "plane",
                a->addr, aircraft_extra_info(a));
  return (true);
}

bool airports_API_flight_log_resolved (const aircraft *a)
{
  char         plane_buf [100];
  char         comment [100];
  bool         fixed;
  ULONGLONG    ft_diff;
  double       msec;
  flight_info *f;

  assert (a->is_helicopter == false);

  f = find_by_callsign (a->call_sign, &fixed);
  if (!f)
     return (false);

  snprintf (plane_buf, sizeof(plane_buf), "plane %06X, call-sign: %s", a->addr, f->call_sign);

  if (f->type == AIRPORT_API_LIVE || f->type == AIRPORT_API_CACHED)
  {
    const char    *dep_location = airport_find_location (f->departure);
    const char    *dst_location = airport_find_location (f->destination);
    const wchar_t *dep_location_w = NULL;
    const wchar_t *dst_location_w = NULL;

    if (f->type == AIRPORT_API_CACHED)
    {
      const char *when;

      if (fixed)
      {
#if defined(USE_BIN_FILES)
        when = "from .BIN-file";
#else
        when = "at startup";
#endif
      }
      else
        when = modeS_FILETIME_to_loc_str (&f->created, true);
      snprintf (comment, sizeof(comment), "cached: %s", when);
    }
    else
    {
      ft_diff = *(ULONGLONG*) &f->responded - *(ULONGLONG*) &f->created;
      msec    = (double)ft_diff / 1E4;   /* 100 nsec units to msec */

      snprintf (comment, sizeof(comment), "%.3lf ms", msec);
    }

    /* Convert both `dep_location` and `dst_location` from UTF-8 to wide-char
     */
    dep_location_w = dep_location ? u8_format (dep_location, 0) : NULL;
    dst_location_w = dst_location ? u8_format (dst_location, 0) : NULL;

    LOG_FILEONLY ("%s, \"%s -> %s\" (%S -> %S, %s).\n",
                  plane_buf, f->departure, f->destination,
                  dep_location_w ? dep_location_w : L"?",
                  dst_location_w ? dst_location_w : L"?",
                  comment);

    if (Modes.speech_enable)
    {
      if (dep_location_w && dst_location_w)
           speak_string ("%s entering. Route %S to %S", f->call_sign, dep_location_w, dst_location_w);
      else speak_string ("%s entering", f->call_sign);
    }
    return (true);
  }

  if (f->type == AIRPORT_API_DEAD)
  {
    if (f->http_status > 0)
    {
      ft_diff = *(ULONGLONG*) &f->responded - *(ULONGLONG*) &f->created;
      msec    = (double)ft_diff / 1E4;
      snprintf (comment, sizeof(comment), " (HTTP %d, %.3lf msec)", f->http_status, msec);
    }
    LOG_FILEONLY ("%s, ? -> ?%s.\n", plane_buf, comment);
    return (true);
  }

  return (false);
}

/*
 * This function logs when an airplane leaves our list of seen aircrafts.
 * But it's possible that the plane reenters.
 */
bool airports_API_flight_log_leaving (const aircraft *a)
{
  flight_info *f = flight_info_find_by_addr (a->addr);
  const char  *km_nmiles = "Nm";
  const char  *m_feet    = "ft";
  const char  *call_sign = "?";
  const char  *mil_cntry = NULL;
  char         mil_buf [20] = { '\0' };
  uint64_t     now = MSEC_TIME();
  double       seconds;
  int          altitude = a->altitude;
  double       alt_div = 1.0;
  char         alt_buf [10] = "-";
  char         active [20];

  if (Modes.metric)
  {
    km_nmiles = "km";
    m_feet    = "m";
    alt_div   = 3.2828;
  }
  if (altitude >= 1)
  {
    altitude = (int) round ((double)a->altitude / alt_div);
    _itoa (altitude, alt_buf, 10);
  }

  if (a->call_sign[0])
     call_sign = a->call_sign;
  else if (f && f->call_sign[0])
     call_sign = f->call_sign;    /* A cached plane or helicopter */

  if (Modes.log && aircraft_is_military(a->addr, &mil_cntry))
  {
    if (mil_cntry)
         snprintf (mil_buf, sizeof(mil_buf), ", Military/%.2s", mil_cntry);
    else strcpy (mil_buf, ", Military");
  }

  seconds = (double) (now - a->seen_first) / 1000.0;
  if (seconds >= 60.0)
       snprintf (active, sizeof(active), "%.0lf min", seconds / 60.0);
  else snprintf (active, sizeof(active), "%.1lf s", seconds);

  LOG_FILEONLY ("%s %06X leaving. call-sign: %s%s, active for %s, alt: %s %s, dist: %s/%s (EST) %s%s\n",
                a->is_helicopter ? "helicopter" : "plane",
                a->addr, call_sign, mil_buf, active, alt_buf, m_feet,
                a->distance_buf[0]     ? a->distance_buf     : "-",
                a->distance_buf_EST[0] ? a->distance_buf_EST : "-", km_nmiles,
                aircraft_extra_info(a));

  if (aircraft_valid(a) && Modes.speech_enable)
  {
    if (a->is_helicopter)
         speak_string ("Helicopter %06X leaving", a->addr);
    else speak_string ("%splane %s leaving",
                       aircraft_is_military(a->addr, NULL) ? "Military " : "",
                       (call_sign[0] == '?') ? "unknown" : call_sign);
  }
  return (true);

  /**
   * \todo
   * Based on current speed, heading and position, figure out:
   *  1) the distance + time flown from the departure airport.
   *  2) the distance + remaining flight-time to the destination airport.
   *
   * Add a `find_airport_pos_by_ICAO (&dest_pos)` function and use
   * `distance = geo_great_circle_dist (&a->position, &dest_pos)`.
   */
}
