/**
 * \file    airports.c
 * \ingroup Main
 * \brief   Handling of airport data from .CSV files
 *          or from ADSB-LOL API. <br>
 *          \ref https://api.adsb.lol
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <locale.h>

#include "misc.h"
#include "interactive.h"
#include "airports.h"

#define API_URL_FMT        "https://api.adsb.lol/api/0/route/%s"
#define API_SERVICE_503    "<html><head><title>503 Service Temporarily Unavailable"
#define AIRPORT_IATA_JSON  "\"_airport_codes_iata\": "

#define AIRPORT_PRINT_HEADER(use_usec) \
        airport_print_header (__LINE__, use_usec)

/**
 * \enum airport_t
 * The source type for an \ref airport or \ref flight_info record.
 */
typedef enum airport_t {
        AIRPORT_CSV = 1,
        AIRPORT_API_LIVE,
        AIRPORT_API_CACHED,
        AIRPORT_API_EXPIRED,  /**\todo Expire cached entries */
        AIRPORT_API_PENDING,
        AIRPORT_API_DEAD
      } airport_t;

/**
 * \typedef airport
 *
 * Describes an airport. Data can be from these sources:
 *  \li A .CSV-file         (`Mode.airport_db == "airport-codes.csv"`)
 *  \li A live API request.
 *  \li A cached API request (`Mode.airport_api_cache == "%TEMP%\\airport-api-cache.csv"`).
 *
 * These are NOT on the same order as in `airport-codes.csv`. <br>
 * CSV header:
 * `# ICAO, IATA, Full_name, Continent, Location, Longitude, Latitude`
 */
typedef struct airport {
        char            ICAO [10];       /**< ICAO code */
        char            IATA [10];       /**< IATA code */
        char            continent [3];   /**< ISO-3166 2 letter continent code */
        char            location  [30];  /**< location or city */
        char            full_name [50];  /**< Full name */
        pos_t           pos;             /**< latitude & longitude */
        airport_t       type;            /**< source of this record */
        struct airport *next;            /**< next airport in our linked list */
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
        airport_t           type;              /**< the type of this record */
        FILETIME            created;           /**< time when this record was created and requested */
        FILETIME            responded;         /**< time when this record had a response */
        struct flight_info *next;              /**< next flight in our linked list */
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
        uint32_t  unknown;
      } flight_info_stats;

/**
 * \typedef airports_stats
 *
 * Statistics for airports handling.
 */
typedef struct airports_stats {
        uint32_t  CSV_numbers;
        uint32_t  CSV_num_ICAO;
        uint32_t  CSV_num_IATA;
        uint32_t  CSV_no_mem;
        uint32_t  API_no_mem;
        uint32_t  API_requests_sent;
        uint32_t  API_requests_recv;
        uint32_t  API_service_503;
        uint32_t  API_added_CSV;
        uint64_t  API_empty_call_sign;
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
        airport          *airports;       /**< Linked list of airports. */
        airport          *airport_CSV;    /**< List of airports sorted on ICAO address. From CSV-file only. */
        char            **IATA_to_ICAO;   /**< List of IATA to ICAO airport codes sorted on IATA address. */
        const char       *usec_fmt;       /**< format used in `airport_print_header()` and `airport_print_rec()`. */
        flight_info      *flight_info;    /**< Linked list of flights information. */
        airport_freq     *freq_CSV;       /**< List of airport freuency information. Not yet. */
        CSV_context       csv_ctx;        /**< Structure for the CSV parser. */
        airports_stats    ap_stats;       /**< Accumulated statistics for airports. */
        flight_info_stats fs_stats;       /**< Accumulated statistics for flight-info. */
        uintptr_t         thread_hnd;     /**< Thread-handle from `_beginthreadex()` */
        unsigned          thread_id;      /**< Thread-ID from `_beginthreadex()` */
        bool              do_trace;       /**< call `API_trace()`? */

        /**
         * \todo
         * Add the WinInet handles here.
         * Or use mg_http_connect() + event handler in dump1090.c.
         *
         * HINTERNET h1;
         * HINTERNET h2;
         */
     }  airports_priv;

static void         airport_CSV_test_1 (void);
static void         airport_CSV_test_2 (bool random);
static void         airport_CSV_test_3 (bool random);
static void         airport_API_test (void);
static void         API_trace (unsigned line, const char *fmt, ...);

static void         locale_test (void);
static void         airport_print_header (unsigned line, bool use_usec);
static void         airport_print_rec (const char *ICAO, const airport *a, size_t idx, double val);

static void         flight_info_exit (FILE *f);
static flight_info *flight_info_create (const char *call_sign, airport_t type);
static void         flight_stats_now (flight_info_stats *stats);
static void         flight_stats_all (flight_info_stats *stats);

static airports_priv g_data;

#define API_TRACE(fmt, ...) do {                                         \
                              if (g_data.do_trace)                       \
                                 API_trace (__LINE__, fmt, __VA_ARGS__); \
                            } while (0)

/*
 * Using the ADSB-LOL API requesting "Route Information" for a call-sign.
 *
 * E.g. Request for call-sign "SAS4787", get JSON-data from:
 * https://api.adsb.lol/api/0/route/SAS4787
 *
 * Response: {
 *   "_airport_codes_iata": "OSL-KEF", << !! 'grep' for this
 *   "_airports": [                    << !! Departure airport
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
 *     {                               << !! Destination airport
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
 *
 * Similar to `curl.exe -s https://api.adsb.lol/api/0/route/SAS4787 | grep "_airport_codes_iata"`
 *
 * Ref: https://api.adsb.lol/docs#/v0/api_route_api_0_route__callsign__get
 */

/**
 * Add an airport record to `g_data.airport_CSV`.
 */
static int CSV_add_entry (const airport *rec)
{
  static airport *copy = NULL;
  static airport *dest = NULL;
  static airport *hi_end;

  if (!copy)  /* Create the initial buffer */
  {
    copy   = dest = malloc (ONE_MEGABYTE);
    hi_end = copy + (ONE_MEGABYTE / sizeof(*rec));
  }
  else if (dest == hi_end - 1)
  {
    size_t new_num = 10000 + g_data.ap_stats.CSV_numbers;

    copy   = realloc (g_data.airport_CSV, sizeof(*rec) * new_num);
    dest   = copy + g_data.ap_stats.CSV_numbers;
    hi_end = copy + new_num;
  }

  if (!copy)
  {
    g_data.ap_stats.CSV_no_mem++;
    return (0);
  }

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
    strncpy (rec.full_name, value, sizeof(rec.full_name)-1);
  }
  else if (ctx->field_num == 3)  /* "Continent" field */
  {
    strncpy (rec.continent, value, sizeof(rec.continent)-1);
  }
  else if (ctx->field_num == 4)  /* "Location" (or City) field */
  {
    strncpy (rec.location, value, sizeof(rec.location)-1);
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

static uint32_t num_lookups, num_hits, num_misses;
static double hit_rate;

/**
 * The compare function for `qsort()` and `bsearch()`.
 */
static int CSV_compare_on_ICAO (const void *_a, const void *_b)
{
  const airport *a = (const airport*) _a;
  const airport *b = (const airport*) _b;
  int   rc = stricmp (a->ICAO, b->ICAO);

  num_lookups++;
  if (rc)
       num_misses++;
  else num_hits++;
  return (rc);
}

static int CSV_compare_on_IATA (const void *a, const void *b)
{
  return stricmp ((const char*)a, (const char*)b);
}

/**
 * Do a binary search for an ICAO airport-name in `g_data.airport_CSV`.
 */
static const airport *CSV_lookup_ICAO (const char *ICAO)
{
  const airport *a = NULL;
  airport        key;

  num_lookups = num_hits = num_misses = 0;
  hit_rate = 0.0;

  if (g_data.airport_CSV)
  {
    strncpy (key.ICAO, ICAO, sizeof(key.ICAO)-1);
    a = bsearch (&key, g_data.airport_CSV, g_data.ap_stats.CSV_numbers,
                 sizeof(*g_data.airport_CSV), CSV_compare_on_ICAO);
    hit_rate = 1.0 - ((double)(num_lookups-1) / (double)g_data.ap_stats.CSV_numbers);
    hit_rate *= 100.0;
  }
  return (a);
}

/**
 * Do a binary search for an IATA to ICAO airport-name mapping.
 */
static const char *IATA_to_ICAO (const char *IATA)
{
  const char *key;

  if (!g_data.IATA_to_ICAO || !IATA)
     return (NULL);

  key = IATA;
  return bsearch (key, g_data.IATA_to_ICAO, g_data.ap_stats.CSV_numbers,
                  sizeof(*g_data.IATA_to_ICAO), CSV_compare_on_IATA);
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
 * Open and parse 'airport-codes.cvs' into the linked list `g_data.airport_CSV`.
 *
 * Also create a mapping of "IATA to ICAO" names (`g_data.IATA_to_ICAO`).
 */
static bool airports_init_CSV (void)
{
  airport *a;
  double   start_t = get_usec_now();
  uint32_t i;

  memset (&g_data.csv_ctx, '\0', sizeof(g_data.csv_ctx));
  g_data.csv_ctx.file_name  = Modes.airport_db;
  g_data.csv_ctx.delimiter  = ',';
  g_data.csv_ctx.callback   = CSV_callback;
  g_data.csv_ctx.num_fields = 7;

  if (!CSV_open_and_parse_file(&g_data.csv_ctx))
  {
    LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.airport_db, strerror(errno));
    return (false);
  }

  TRACE ("Parsed %u records in %.3f msec from: \"%s\"",
         g_data.ap_stats.CSV_numbers, (get_usec_now() - start_t) /1E3, Modes.airport_db);

  if (g_data.ap_stats.CSV_numbers > 0)
  {
    a = g_data.airport_CSV + 0;
    num_lookups = num_hits = num_misses = 0;
    qsort (g_data.airport_CSV, g_data.ap_stats.CSV_numbers, sizeof(*g_data.airport_CSV),
           CSV_compare_on_ICAO);

    for (i = 0; i < g_data.ap_stats.CSV_numbers; i++, a++)
    {
      a->type = AIRPORT_CSV;
      LIST_ADD_TAIL (airport, &g_data.airports, a);
    }

    g_data.IATA_to_ICAO = malloc (sizeof(char*) * g_data.ap_stats.CSV_numbers);
    if (g_data.IATA_to_ICAO)
    {
      for (i = 0, a = g_data.airports; a; a = a->next, i++)
          g_data.IATA_to_ICAO [i] = a->IATA;
      qsort (g_data.IATA_to_ICAO, g_data.ap_stats.CSV_numbers, sizeof(*g_data.IATA_to_ICAO),
             CSV_compare_on_IATA);
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
 * Open and parse `Modes.airport_freq_db` into the linked list `g_data.airport_freq_CSV`
 */
static bool airports_init_freq_CSV (void)
{
  return (true);
}

static void airports_exit_freq_CSV (void)
{
}

static uint32_t airports_numbers (airport_t type)
{
  const airport *a;
  uint32_t       num =0;

  for (a = g_data.airports; a; a = a->next)
      if (a->type == type)
         num++;
  return (num);
}

/**
 * Return the number of dynamic airport records.
 */
static uint32_t airports_numbers_API (void)
{
  return (airports_numbers(AIRPORT_API_LIVE) +
          airports_numbers(AIRPORT_API_CACHED));
}

/**
 * Add a cached flight-info record to `g_data.flight_info`.
 */
static int API_add_entry (const flight_info *rec)
{
  flight_info *f = flight_info_create (rec->call_sign, AIRPORT_API_CACHED);

  if (f)
  {
    f->created = rec->created;
    strcpy (f->departure, rec->departure);
    strcpy (f->destination, rec->destination);
    g_data.ap_stats.API_added_CSV++;
  }
  return (f ? 1 : 0);
}

static int API_cache_callback (struct CSV_context *ctx, const char *value)
{
  static flight_info rec;
  int    rc = 1;

  if (ctx->field_num == 0)
     strncpy (rec.call_sign, value, sizeof(rec.call_sign)-1);

  else if (ctx->field_num == 1)
     strncpy (rec.departure, value, sizeof(rec.departure)-1);

  else if (ctx->field_num == 2)
     strncpy (rec.destination, value, sizeof(rec.destination)-1);

  else if (ctx->field_num == 3)
  {
    ULARGE_INTEGER *ul;
    char           *end;
    uint64_t        val = strtoull (value, &end, 10);

    if (end > value)
    {
      ul = (ULARGE_INTEGER*) &val;
      rec.created = *(FILETIME*) &ul->QuadPart;
    }
    rc = API_add_entry (&rec);
    memset (&rec, '\0', sizeof(rec));    /* ready for a new record. */
  }
  return (rc);
}

static int API_compare_on_dest_airport (const void *a, const void *b)
{
  MODES_NOTUSED (a);
  MODES_NOTUSED (b);
  return (0);
}

static void API_trace (unsigned line, const char *fmt, ...)
{
  char    buf [200], *ptr = buf;
  int     len, left = (int)sizeof(buf);
  va_list args;

  len = snprintf (ptr, left, "%s(%u, %s): ",
                  Modes.tests ? "" : "airports.c",
                  line, GetCurrentThreadId() == g_data.thread_id ?
                  "API-thread" : "main-thread");
  ptr  += len;
  left -= len;

  va_start (args, fmt);
  vsnprintf (ptr, left, fmt, args);
  va_end (args);
  modeS_flogf (stdout, "%s\n", buf);
}

/**
 * Return 'true' if we have PENDING records to resolve.
 */
static bool API_have_pending (void)
{
  const flight_info *f;

  for (f = g_data.flight_info; f; f = f->next)
      if (f->type == AIRPORT_API_PENDING)
         return (true);
  return (false);
}

/**
 * Dump records of all types.
 */
static void API_dump_records (void)
{
  const flight_info *f;
  flight_info_stats  fs;
  int    i = 0;

  puts ("   #  Call-sign  DEP -> DEST   Type    Created                   Resp-time (ms)\n"
        "  -----------------------------------------------------------------------------");

  for (f = g_data.flight_info; f; f = f->next)
  {
    ULONGLONG ft_diff;
    char      dtime [20] = "N/A";

    if (*(ULONGLONG*)&f->responded)
    {
      ft_diff = *(ULONGLONG*) &f->responded - *(ULONGLONG*) &f->created;
      snprintf (dtime, sizeof(dtime), "%.3lf", (double)ft_diff / 1E6);
    }
    printf ("  %2d: %-8s   %-5s  %-5s  %-7s %s  %s\n",
            i++, f->call_sign, f->departure, f->destination,
            airport_t_str(f->type), modeS_FILETIME_to_str(&f->created, true), dtime);
  }
  flight_stats_now (&fs);
  printf ("  Total: %u, live: %u, cached: %u\n", fs.total, fs.live, fs.cached);
  puts("");
}

void airports_show_stats (void)
{
  flight_info_stats fs_now, fs_all;

  LOG_STDOUT ("Airports statistics:\n");
  interactive_clreol();

  LOG_STDOUT ("  %6u CSV records in list.\n", g_data.ap_stats.CSV_numbers);
  interactive_clreol();

  LOG_STDOUT ("  %6u API records in list.\n", airports_numbers_API());
  interactive_clreol();

  LOG_STDOUT ("  %6u API requests sent.\n", g_data.ap_stats.API_requests_sent);
  interactive_clreol();

  LOG_STDOUT ("  %6u API requests received.\n", g_data.ap_stats.API_requests_recv);
  interactive_clreol();

  LOG_STDOUT ("  %6u API 503 Service Unavailable.\n", g_data.ap_stats.API_service_503);
  interactive_clreol();

  LOG_STDOUT ("  %6u API live records.\n", g_data.fs_stats.live);
  interactive_clreol();

  LOG_STDOUT ("  %6u API dead records.\n", g_data.fs_stats.dead);
  interactive_clreol();

  LOG_STDOUT ("  %6u dropped due to no memory.\n", g_data.ap_stats.API_no_mem);
  interactive_clreol();

  flight_stats_now (&fs_now);
  flight_stats_all (&fs_all);

  LOG_STDOUT ("  Flight-info, total=%u\n", fs_now.total);
  interactive_clreol();

  LOG_STDOUT ("  %6u / %-6u live.\n", fs_now.live, fs_all.live);
  interactive_clreol();

  LOG_STDOUT ("  %6u / %-6u cached.\n", fs_now.cached, fs_all.cached);
  interactive_clreol();

  LOG_STDOUT ("  %6u / %-6u dead.\n", fs_now.dead, fs_all.dead);
  interactive_clreol();
}

/*
 * This function blocks the api_thread_fn() function.
 *
 * Send one request at a time for a call-sign to be resolved into a
 * `AIRPORT_API_LIVE` flight-record.
 */
static bool API_thread_worker (flight_info *f)
{
  char      *response, *codes;
  char       url [200];
  bool       rc = false;
  SYSTEMTIME st_now;
  FILETIME   ft_now;

  snprintf (url, sizeof(url), API_URL_FMT, f->call_sign);
  g_data.ap_stats.API_requests_sent++;

  API_TRACE ("request # %lu: downloading '%s'\n", g_data.ap_stats.API_requests_sent, url);

  response = download_to_buf (url);  /* This function blocks */
  if (!response)
  {
    API_TRACE ("Downloaded no data for %s!", f->call_sign);
    return (false);
  }

  API_TRACE ("Downloaded %zu bytes data for '%s': '%.*s'...",
             strlen(response), f->call_sign, 50, response);

  GetLocalTime (&st_now);
  SystemTimeToFileTime (&st_now, &ft_now);
  f->responded = ft_now;

  /* We sent too many requests!
   */
  if (!strncmp(response, API_SERVICE_503, sizeof(API_SERVICE_503)-1))
  {
    g_data.ap_stats.API_service_503++;
    return (false);
  }

  g_data.ap_stats.API_requests_recv++;

  codes = strstr (response, AIRPORT_IATA_JSON);
  if (codes && *codes)
  {
    flight_info tmp;
    int         num;

    memset (&tmp, '\0', sizeof(tmp));
    codes += strlen (AIRPORT_IATA_JSON);
    num = sscanf (codes, "\"%30[^-\"]-%30[^-\"]\"",
                  tmp.departure, tmp.destination);

    if (!strcmp(codes, "\"unknown\"") || num != 2)
    {
      g_data.fs_stats.unknown++;
      rc = false;   /* This request becomes a dead record */
    }
    else
    {
      strcpy (f->departure, tmp.departure);
      strcpy (f->destination, tmp.destination);
      rc = true;
    }
    API_TRACE ("num: %d, tmp.departure: '%s', tmp.destination: '%s'",
               num, tmp.departure, tmp.destination);
  }
  free (response);
  return (rc);
}

/**
 * Called from `background_tasks()` 4 times per second.
 */
void airports_API_show_stats (uint64_t now)
{
  flight_info_stats fs;

  flight_stats_now (&fs);
  API_TRACE ("stats now: total=%u, live=%u, pending=%u, dead=%u, unknown=%u",
             fs.total, fs.live, fs.pending, fs.dead, fs.unknown);
  MODES_NOTUSED (now);
}

/**
 * Called from `background_tasks()` 4 times per second.
 *
 * Does nothing yet.
 */
void airports_API_remove_stale (uint64_t now)
{
  MODES_NOTUSED (now);
}

/**
 * The thread for handling flight-info API requests.
 *
 * Got through the `g_data.flight_info` list and handle a
 * `AIRPORT_API_PENDING` record one at a time.
 */
static unsigned int __stdcall api_thread_fn (void *arg)
{
  while (1)
  {
    flight_info *f;

    /* Sleep if queue is empty.
     */
    if (!API_have_pending())
    {
      Sleep (100);
      continue;
    }

    for (f = g_data.flight_info; f; f = f->next)
    {
      if (f->type == AIRPORT_API_PENDING)
      {
        /* Change the state to AIRPORT_API_LIVE even
         * for an error-response like `"_airport_codes_iata": "unknown"`
         */
        if (API_thread_worker(f))
        {
          f->type = AIRPORT_API_LIVE;
          g_data.fs_stats.live++;
          g_data.fs_stats.pending--;
        }
        else   /* Otherwise it's a dead record */
        {
          f->type = AIRPORT_API_DEAD;
          g_data.fs_stats.dead++;
        }
      }
    }
  }
  MODES_NOTUSED (arg);
  return (0);
}

/**
 * Open for writing or create the `%TEMP%\\ AIRPORT_DATABASE_CACHE` file.
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

  fputs ("# callsign,departure,destination,timestamp\n", f);
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
         API_TRACE ("\"%s\": %u bytes written.", Modes.airport_cache, st.st_size);
    else API_TRACE ("\"%s\": errno: %d/%s.", Modes.airport_cache, errno, strerror(errno));
  }
  if (!f && Modes.tests > 0)
     printf ("No need to rewrite the %s cache.\n", Modes.airport_cache);
}

/**
 * Open and parse the `%TEMP%\\ AIRPORT_DATABASE_CACHE` file
 * and append to `g_data.flight_info`.
 *
 * These records are always `a->type == AIRPORT_API_CACHED`.
 */
static bool airports_init_API (void)
{
  struct stat st;
  bool        exists;

  g_data.thread_hnd = _beginthreadex (NULL, 0, api_thread_fn, NULL, 0, &g_data.thread_id);
  if (!g_data.thread_hnd)
  {
    LOG_STDERR ("Failed to create thread: %s\n", strerror(errno));
    return (false);
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
    g_data.csv_ctx.num_fields = 4;

    if (!CSV_open_and_parse_file(&g_data.csv_ctx))
    {
      API_TRACE ("c_in: 0x%02X, state: %d", g_data.csv_ctx.c_in, g_data.csv_ctx.state);

      /* getting only the header from a previous run is not an error
       */
      if (g_data.csv_ctx.state != STATE_EOF &&
          g_data.csv_ctx.state != STATE_NORMAL)
      {
        LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.airport_cache, strerror(errno));
        return (false);
      }
    }

    flight_stats_now (&fs);
    TRACE ("Parsed %u/%u records from: \"%s\"", fs.cached, g_data.ap_stats.API_added_CSV, Modes.airport_cache);

    if (Modes.tests)
       assert (fs.cached == g_data.ap_stats.API_added_CSV);

    if (g_data.ap_stats.API_added_CSV > 0)
       qsort (g_data.airports, g_data.ap_stats.API_added_CSV,
              sizeof(*g_data.airports), API_compare_on_dest_airport);
  }
  return (true);
}

/**
 * Main init function for this module.
 *
 * Called from:
 *  \li modeS_init() for `--test` mode or from
 *  \li interactive_init() in `--interactive` mode.
 */
bool airports_init (void)
{
  int  test_lvl = (Modes.tests_arg ? Modes.tests_arg : 1);
  bool rc;

  assert (g_data.airports == NULL);

  if (Modes.tests)
  {
    g_data.do_trace = true;
    Modes.debug |= DEBUG_GENERAL;
    TRACE ("test_lvl: %d", test_lvl);
  }

  Modes.airports_priv = &g_data;

  rc = (airports_init_CSV() &&
        airports_init_freq_CSV() &&
        airports_init_API());

  if (Modes.tests)
  {
    const  airport *a;
    size_t i, i_max = (test_lvl >= 2 ? g_data.ap_stats.CSV_numbers : 10);

    printf ("%s(), Dumping %zu airport records: ", __FUNCTION__, i_max);
    AIRPORT_PRINT_HEADER (false);

    for (i = 0, a = g_data.airports; a && i < i_max; a = a->next, i++)
        airport_print_rec (a->ICAO, a, i, 0.0);
    puts ("");

    airport_CSV_test_1();
    airport_CSV_test_2 (true);
    airport_CSV_test_3 (true);
    airport_API_test();
    airports_show_stats();

    if (test_lvl >= 2)
       locale_test();

    rc = false;              /* Just force an exit */
  }
  return (rc);
}

/**
 * Free the `g_data.airports` linked list
 * \todo
 * Close down the WinInet API and rewrite the API file-cache.
 */
static void airports_exit_API (void)
{
  airport *a, *a_next;

  for (a = g_data.airports; a; a = a_next)
  {
    a_next = a->next;
    LIST_DELETE (airport, &g_data.airports, a);
  }

  g_data.airports = NULL;

  free (g_data.IATA_to_ICAO);
  g_data.IATA_to_ICAO = NULL;

  if (g_data.thread_hnd)
     CloseHandle ((HANDLE)g_data.thread_hnd);
  g_data.thread_hnd = 0ULL;
}

void airports_exit (void)
{
  airports_exit_API();
  airports_exit_CSV();
  airports_exit_freq_CSV();
  airports_cache_write();
  Modes.airports_priv = NULL;
}

/**
 * Dumping of airport data:
 *  \li print a starting header.
 *  \li print the actual record for an `airport*`
 */
static void airport_print_header (unsigned line, bool use_usec)
{
  g_data.usec_fmt = use_usec ? "%.2f" : "%.2f%%";

  printf ("line: %u:\n"
          "  Rec  ICAO       IATA       cont location               full_name                                                   lat       lon  %s\n"
          "  --------------------------------------------------------------------------------------------------------------------------------------------\n",
          line, use_usec ? "usec" : "hit-rate");
}

static void airport_print_rec (const char *ICAO, const airport *a, size_t idx, double val)
{
  static const pos_t pos0 = { 0.0, 0.0 };
  char   usec_buf [20] = { "-" };

  const char  *IATA      = a ? a->IATA      : "?";
  const char  *full_name = a ? a->full_name : "?";
  const char  *continent = a ? a->continent : "?";
  const char  *location  = a ? a->location  : "?";
  const pos_t *pos       = a ? &a->pos      : &pos0;

  if (val > 0.0)
     snprintf (usec_buf, sizeof(usec_buf), g_data.usec_fmt, val);

  printf ("%5zu  '%-8.8s' '%-8.8s' %2.2s   '%-20.20s' '%-50.50s'  %9.3f %9.3f  %s\n",
          idx, ICAO, IATA, continent, location, full_name, pos->lat, pos->lon, usec_buf);
}

#define ADD_AIRPORT(ICAO, IATA, continent, location, \
                    full_name, lon, lat)             \
                 { { ICAO },                         \
                   { IATA },                         \
                   { continent },                    \
                   { location  },                    \
                   { full_name },                    \
                   { lon, lat  },                    \
                   AIRPORT_CSV,                      \
                   NULL                              \
                 }

#if defined(__clang__)
  #pragma clang diagnostic push    "-Winvalid-source-encoding"
  #pragma clang diagnostic ignored "-Winvalid-source-encoding"
#endif

static const airport airport_tests [] = {
       ADD_AIRPORT ("ENBR", "BGO", "EU", "Bergen",       "Bergen Airport Flesland",                5.218140120, 60.293399810000),
       ADD_AIRPORT ("ENGM", "OSL", "EU", "Oslo",         "Oslo Gardermoen Airport",               11.100399971, 60.193901062012),
       ADD_AIRPORT ("KJFK", "JFK", "NA", "New York",     "John F Kennedy International Airport", -73.778000000, 40.639801000000),
       ADD_AIRPORT ("OTHH", "DOH", "AS", "Doha",         "Hamad International Airport",           51.608050000, 25.273056000000),
       ADD_AIRPORT ("AF10", "URZ", "AS", "OrÅ«zgÄ\\x81n","OrÅ«zgÄ\\x81n Airport",                 66.630897520, 32.9029998779000) /* Uruzgan / Afghanistan */
     };

#if defined(__clang__)
  #pragma clang diagnostic pop "-Winvalid-source-encoding"
#endif

/**
 * Do some simple tests on the `g_data.airport_CSV`.
 */
static void airport_CSV_test_1 (void)
{
  const airport *a;
  const airport *t = airport_tests + 0;
  size_t         i, num_ok;

  printf ("%s():\n", __FUNCTION__);

  printf ("  Checking %zu fixed records against \"%s\". ",
          DIM(airport_tests), basename(Modes.airport_db));

  AIRPORT_PRINT_HEADER (false);

  for (i = num_ok = 0; i < DIM(airport_tests); i++, t++)
  {
    a = CSV_lookup_ICAO (t->ICAO);
    if (a && !memcmp(a->location, t->location, sizeof(t->location)))
       num_ok++;
    airport_print_rec (t->ICAO, a, i, hit_rate);
  }
  printf ("\n"
          "  %3zu OKAY\n", num_ok);
  printf ("  %3zu FAIL\n", i - num_ok);
  puts ("");
}

static void airport_CSV_test_2 (bool random)
{
  const airport *a;
  size_t         i, num;

  printf ("%s (%s):\n", __FUNCTION__, random ? "true" : "false");

  if (!g_data.airport_CSV)
     return;

  if (Modes.tests_arg)
       num = Modes.tests_arg;
  else num = 10;

  if (num >= g_data.ap_stats.CSV_numbers)
     num = g_data.ap_stats.CSV_numbers;

  printf ("  Checking %zu %s records. ", num, random ? "random" : "fixed");
  AIRPORT_PRINT_HEADER (true);

  for (i = 0; i < num; i++)
  {
    const char *ICAO;
    double      usec;
    size_t      rec_num;

    if (random)
         rec_num = (size_t) random_range (0, g_data.ap_stats.CSV_numbers - 1);
    else rec_num = i;

    usec  = get_usec_now();
    ICAO  = g_data.airport_CSV [rec_num].ICAO;
    a     = CSV_lookup_ICAO (ICAO);
    usec  = get_usec_now() - usec;
    airport_print_rec (ICAO, a, rec_num, usec);
  }
  puts ("");
}

static void airport_CSV_test_3 (bool random)
{
  size_t i, num = 10;

  printf ("%s (%s):\n", __FUNCTION__, random ? "true" : "false");
  printf ("  Checking %zu %s records. ", num, random ? "random" : "fixed");

  puts ("    Rec  ICAO      ICAO2       full_name\n"
        "  -------------------------------------------------------------------");

  for (i = 0; i < num; i++)
  {
    const airport *a;
    const char    *ICAO, *ICAO2;
    unsigned       rec_num;

    if (random)
         rec_num = random_range (0, g_data.ap_stats.CSV_numbers - 1);
    else rec_num = i;

    ICAO = g_data.airport_CSV [rec_num].ICAO;
    a = CSV_lookup_ICAO (ICAO);
    if (a && a->IATA[0])
         ICAO2 = IATA_to_ICAO (a->IATA);
    else ICAO2 = NULL;
    printf ("  %5u '%-8s' '%-8s' '%s'\n",
            rec_num, ICAO, ICAO2 ? ICAO2 : "?",
            a ? a->full_name : "?");
  }
  puts ("");
}

/**
 * Do a simple test on the `g_data.airports` created by
 * `airports_init_API()`
 */
static void airport_API_test (void)
{
  flight_info_stats fs;
  int         i, m_sec = 0;
  bool        rc, save = g_data.do_trace;
  bool        pending_complete;
  const char *departure, *destination;
  const char *call_signs[] = {
             "AAL292",  "SK293",
             "TY15",    "WIF17T",
             "CFG2092", "NOZ8LE"
           };

  g_data.do_trace  = false;
  pending_complete = false;

  printf ("%s(),  lookup phase:\n", __FUNCTION__);

  for (i = 0; i < (int)DIM(call_signs); i++)
     airports_API_get_flight_info (call_signs[i], &departure, &destination);

  API_dump_records();

  while (API_have_pending())
  {
    printf ("  %3d: Waiting for thread %u to complete.\n", m_sec, g_data.thread_id);
    Sleep (100);
    m_sec += 100;
    pending_complete = true;
  }

  flight_stats_now (&fs);
  printf ("  Num-live: %u, Num-cached: %u.\n", fs.live, g_data.ap_stats.API_added_CSV);

  if (pending_complete)
  {
    puts ("  Results now:");
    API_dump_records();
  }

  puts ("  Testing IATA to ICAO lookup:");

  for (i = 0; i < (int)DIM(call_signs); i++)
  {
    rc = airports_API_get_flight_info (call_signs[i], &departure, &destination);

    const airport *ICAO_airport;
    const char    *ICAO_departure   = IATA_to_ICAO (departure);
    const char    *ICAO_destination = IATA_to_ICAO (destination);

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

    printf ("    %-8s: %-8s -> %-8s (%s - %s)\n",
            call_signs[i], departure, destination,
            ICAO_departure   ? ICAO_departure   : "?",
            ICAO_destination ? ICAO_destination : "?");
  }

  g_data.do_trace = save;
  puts ("");
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
 * Created a linked list of pending, live or cached flight-information.
 */
static flight_info *flight_info_create (const char *call_sign, airport_t type)
{
  SYSTEMTIME   st_now;
  FILETIME     ft_now;
  flight_info *f = calloc (sizeof(*f), 1);

  if (!f)
  {
    g_data.ap_stats.API_no_mem++;
    return (NULL);
  }

  GetLocalTime (&st_now);
  SystemTimeToFileTime (&st_now, &ft_now);

  strncpy (f->call_sign, call_sign, sizeof(f->call_sign)-1);
  f->type    = type;
  f->created = ft_now;
  strcpy (f->departure, "?");
  strcpy (f->destination, "?");

  LIST_ADD_TAIL (flight_info, &g_data.flight_info, f);

  g_data.fs_stats.total++;
  switch (type)
  {
    case AIRPORT_API_PENDING:
         g_data.fs_stats.pending++;
         break;
    case AIRPORT_API_CACHED:
         g_data.fs_stats.cached++;
         break;
    default:
         assert (0);
         break;
  }
  return (f);
}

/**
 * Traverse the `g_data.flight_info` list to get flight-information for this call-sign.
 */
static flight_info *flight_info_find (const char *call_sign)
{
  flight_info *f;

  for (f = g_data.flight_info; f; f = f->next)
  {
    if (!stricmp(f->call_sign, call_sign))
       return (f);
  }
  return (NULL);
}

/**
 * Write the a `g_data.flight_info` element to file-cache.
 */
static void flight_info_write (FILE *file, const flight_info *f)
{
  const char *departure    = f->departure;
  const char *destination  = f->destination;
  const ULARGE_INTEGER *ul = (const ULARGE_INTEGER*) &f->created;

  if (!stricmp(f->departure, "unknown"))
     departure = "?";
  if (!stricmp(f->destination, "unknown"))
     destination = "?";

  fprintf (file, "%s,%s,%s,%llu\n",
           f->call_sign, departure, destination, ul->QuadPart);
}

/**
 * Exit function for flight-info:
 *  \li Write the `AIRPORT_API_CACHED` or `AIRPORT_API_LIVE` records to `Modes.airport_cache`.
 *  \li Free the `g_data.flight_info` list.
 */
static void flight_info_exit (FILE *file)
{
  flight_info *f, *f_next;

  for (f = g_data.flight_info; f; f = f_next)
  {
    if (file)
       flight_info_write (file, f);

    f_next = f->next;
    LIST_DELETE (flight_info, &g_data.flight_info, f);
    free (f);

  }
  g_data.flight_info = NULL;
}

/**
 * Return counters for flight-info active now.
 */
static void flight_stats_now (flight_info_stats *fs)
{
  const flight_info *f;

  memset (fs, '\0', sizeof(*fs));

  for (f = g_data.flight_info; f; f = f->next)
  {
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
      default:
           TRACE ("record %u: Unknown f->type: %d", fs->total, f->type);
           break;
    }
  }
}

/**
 * Return counters for accumulated flight-info.
 */
static void flight_stats_all (flight_info_stats *fs)
{
  memcpy (fs, &g_data.fs_stats, sizeof(*fs));
}

/**
 * Non-blocking function called to get flight-information for a single call-sign.
 *
 * Add to the API lookup-queue if not already in the `g_data.flight_info` linked list.
 * \param departure   [in|out]  a pointer to the IATA departure airport.
 * \param destination [in|out]  a pointer to the IATA destianation airport.
 */
bool airports_API_get_flight_info (const char  *call_sign,
                                   const char **departure,
                                   const char **destination)
{
  const char  *type;
  flight_info *f;
  const char   *end;

  *departure = *destination = NULL;

  if (*call_sign == '\0')
  {
    API_TRACE ("Empty 'call_sign'!");
    g_data.ap_stats.API_empty_call_sign++;
    return (false);
  }

  end = strrchr (call_sign, '\0');
  assert (end[-1] != ' ');

  f = flight_info_find (call_sign);
  if (!f)
  {
    flight_info_create (call_sign, AIRPORT_API_PENDING);
    API_TRACE ("Created pending record for call_sign: '%s'", call_sign);
    return (false);
  }

  type = airport_t_str (f->type);

  if (f->type == AIRPORT_API_LIVE || f->type == AIRPORT_API_CACHED)
  {
    API_TRACE ("call_sign: '%s', type: %s, '%s' -> '%s'", call_sign, type, f->departure, f->destination);
    *departure   = f->departure;
    *destination = f->destination;
    return (true);
  }

  API_TRACE ("call_sign: '%s', type: %s, ? -> ?", call_sign, type);
  return (false);
}
