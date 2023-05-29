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
#include "airports.h"

static void     airport_CSV_test_1 (void);
static void     airport_CSV_test_2 (void);
static void     airport_API_test (void);
static uint32_t airports_numbers (airport_t type);

static void     locale_test (void);
static void     flight_info_exit (void);
static void     flight_info_test (void);
static uint32_t flight_numbers (uint32_t *live_p, uint32_t *cached_p);

static void     airport_print_header (unsigned line, bool use_usec);
static void     airport_print_rec (const char *ICAO, const airport *a, size_t idx, double val);

#define AIRPORT_PRINT_HEADER(use_usec)  airport_print_header (__LINE__, use_usec)

static const char   *g_usec_fmt;
static flight_info  *g_flight_info      = NULL;
static airport_freq *g_airport_freq_CSV = NULL;

/*
 * Using the ADSB-LOL API requesting "Route Information" for a call-sign.

 * E.g. Request for call-sign "SAS4787", get JSON-data from:
 * https://api.adsb.lol/api/0/route/SAS4787
 *
 * Response: {
 *   "_airport_codes_iata": "OSL-KEF", << !! 'grep' for this; mg_json_get_str (buf, "_airport_codes_iata");
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
 * Add an airport record to `Modes.airport_list_CSV`.
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
    size_t new_num = 10000 + Modes.airport_num_CSV;

    copy   = realloc (Modes.airport_list_CSV, sizeof(*rec) * new_num);
    dest   = copy + Modes.airport_num_CSV;
    hi_end = copy + new_num;
  }

  if (!copy)
     return (0);

  Modes.airport_list_CSV = copy;
  assert (dest < hi_end);
  memcpy (dest, rec, sizeof(*rec));
  Modes.airport_num_CSV++;

  dest = copy + Modes.airport_num_CSV;
  return (1);
}

/**
 * The CSV callback for adding a record to `Modes.airport_list_CSV`.
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

/**
 * Do a binary search for an airport in `Modes.airport_list_CSV`.
 */
static const airport *CSV_lookup_entry (const char *ICAO)
{
  const airport *a = NULL;
  airport        key;

  num_lookups = num_hits = num_misses = 0;
  hit_rate = 0.0;

  if (Modes.airport_list_CSV)
  {
    strncpy (key.ICAO, ICAO, sizeof(key.ICAO)-1);
    a = bsearch (&key, Modes.airport_list_CSV, Modes.airport_num_CSV,
                 sizeof(*Modes.airport_list_CSV), CSV_compare_on_ICAO);
    hit_rate = 1.0 - ((double)(num_lookups-1) / (double)Modes.airport_num_CSV);
    hit_rate *= 100.0;
  }
  return (a);
}

static const char *airport_t_str (airport_t type)
{
  static char buf [20];

  if (type == AIRPORT_CSV)
     return ("CSV");
  if (type == AIRPORT_API_LIVE)
     return ("LIVE");
  if (type == AIRPORT_API_CACHED)
     return ("CACHED");

  snprintf (buf, sizeof(buf), "%d?", type);
  return (buf);
}

/*
 * Open and parse 'airport-codes.cvs' into the linked list `Modes.airport_list_CSV`
 */
static bool airports_init_CSV (void)
{
  airport *a;
  double   start_t = get_usec_now();
  uint32_t i;

  memset (&Modes.csv_ctx, '\0', sizeof(Modes.csv_ctx));
  Modes.csv_ctx.file_name  = Modes.airport_db;
  Modes.csv_ctx.delimiter  = ',';
  Modes.csv_ctx.callback   = CSV_callback;
  Modes.csv_ctx.num_fields = 7;

  if (!CSV_open_and_parse_file(&Modes.csv_ctx))
  {
    LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.airport_db, strerror(errno));
    return (false);
  }

  TRACE ("Parsed %u records in %.3f msec from: \"%s\"",
          Modes.airport_num_CSV, (get_usec_now() - start_t) /1E3, Modes.airport_db);

  if (Modes.airport_num_CSV > 0)
  {
    a = Modes.airport_list_CSV + 0;
    num_lookups = num_hits = num_misses = 0;
    qsort (Modes.airport_list_CSV, Modes.airport_num_CSV, sizeof(*Modes.airport_list_CSV),
           CSV_compare_on_ICAO);

    for (i = 0; i < Modes.airport_num_CSV; i++, a++)
    {
      a->type = AIRPORT_CSV;
      LIST_ADD_TAIL (airport, &Modes.airports, a);
    }
  }

  if (Modes.tests)
  {
    int test_lvl = (Modes.tests_arg ? Modes.tests_arg : 1);

    if (test_lvl >= 2 && Modes.airport_num_CSV > 0)
    {
      AIRPORT_PRINT_HEADER (false);
      for (i = 0, a = Modes.airports; a; a = a->next, i++)
          airport_print_rec (a->ICAO, a, i, 0.0);
    }
    if (test_lvl >= 2)
    {
      locale_test();
      airport_CSV_test_1();
      airport_CSV_test_2();
    }
    airport_API_test();
    flight_info_test();
    return (false);    /* Just force an exit */
  }
  return (true);
}

/*
 * Free memory allocated above.
 */
static void airports_exit_CSV (void)
{
  if (Modes.airport_list_CSV)
     free (Modes.airport_list_CSV);

  Modes.airport_list_CSV = NULL;
  Modes.airport_num_CSV  = 0;
}

/**
 * \todo
 * Open and parse `Modes.airport_freq_db` into the linked list `g_airport_freq_CSV`
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

  for (a = Modes.airports; a; a = a->next)
      if (a->type == type)
         num++;
  return (num);
}

/*
 * Return the number of fixed airport records
 */
uint32_t airports_numbers_CSV (void)
{
  uint32_t num = airports_numbers (AIRPORT_CSV);

  assert (num == Modes.airport_num_CSV);
  return (num);
}

/**
 * Return the number of dynamic airport records.
 */
uint32_t airports_numbers_API (void)
{
  return (airports_numbers(AIRPORT_API_LIVE) +
          airports_numbers(AIRPORT_API_CACHED));
}

static int API_callback (struct CSV_context *ctx, const char *value)
{
  TRACE ("rec_num: %u, field_mum: %u, value: %s", ctx->rec_num, ctx->field_num, value);
  return (1);
}

static int API_compare_on_dest_airport (const void *_a, const void *_b)
{
  MODES_NOTUSED (_a);
  MODES_NOTUSED (_b);
  return (0);
}

/**
 * Open and parse the `%TEMP%\\ AIRPORT_DATABASE_CACHE` file
 * and append to `Modes.airports`.
 *
 * These records are always `a->type == AIRPORT_API_CACHED`.
 */
static bool airports_init_API (void)
{
  double       start_t = get_usec_now();
  uint32_t     num;
  FILE        *f;
  struct stat  st;
  bool         exists = true;


#if 0
  // Use 'URLOpenStream()' or 'URLOpenBlockingStream()' in another thread
  // to load the .JSON into a local buffer?

  HRESULT hr = CoInitializeEx (NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

  CoCreateInstance (&CLSID_StdURLMoniker, NULL, CLSCTX_INPROC_SERVER, &IID_IWinInetFileStream, (void**)&g_urlmon);

  if (!SUCCEEDED(hr) || !g_urlmon)
  {
    LOG_STDERR ("Failed to load URLMON.\n");
    return (false);
  }
#endif

  if (stat(Modes.airport_cache, &st) != 0)
  {
    LOG_STDERR ("\nCache '%s' does not exists; creating it.\n", Modes.airport_cache);
    f = fopen (Modes.airport_cache, "w+t");
    if (!f)
    {
      LOG_STDERR ("Failed to create \"%s\": %s\n", Modes.airport_cache, strerror(errno));
      return (false);
    }
    fputs ("#departure,destination,timestamp\n", f);
    fclose (f);
    exists = false;  /* avoid parsing an empty .csv-file */
    return (true);
  }

  memset (&Modes.csv_ctx, '\0', sizeof(Modes.csv_ctx));
  Modes.csv_ctx.file_name = Modes.airport_cache;
  Modes.csv_ctx.delimiter = ',';
  Modes.csv_ctx.callback  = API_callback;
  Modes.csv_ctx.line_size = 2000;

  if (exists && !CSV_open_and_parse_file(&Modes.csv_ctx))
  {
    LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.airport_cache, strerror(errno));
    return (false);
  }

  num = airports_numbers (AIRPORT_API_CACHED);
  TRACE ("Parsed %u records in %.3f msec from: \"%s\"",
         num, (get_usec_now() - start_t) /1E3, Modes.airport_cache);

  if (num > 0)
     qsort (Modes.airports, num, sizeof(*Modes.airports), API_compare_on_dest_airport);

  return (true);
}

bool airports_init (void)
{
  assert (Modes.airports == NULL);

  if (Modes.tests)
     Modes.debug |= DEBUG_GENERAL;

  return (airports_init_CSV() &&
          airports_init_freq_CSV() &&
          airports_init_API());
}

/**
 * Free the `Modes.airports` linked list
 * \todo
 * Close down the WinInet API and rewrite the API file-cache.
 */
static void airports_exit_API (bool free_airports)
{
  airport *a, *a_next;

#if 0
  // CoUninitialize();
#endif

  if (!free_airports)
     return;

  for (a = Modes.airports; a; a = a_next)
  {
    a_next = a->next;
    LIST_DELETE (airport, &Modes.airports, a);
  }
  Modes.airports = NULL;
}

void airports_exit (bool free_airports)
{
  TRACE ("%5u CSV records in list", airports_numbers_CSV());
  TRACE ("%5u API records in list", airports_numbers_API());

  airports_exit_API (free_airports);
  airports_exit_CSV();
  airports_exit_freq_CSV();
  flight_info_exit();

  assert (airports_numbers_CSV() == 0);
  assert (airports_numbers_API() == 0);
}

/**
 * Dumping of airport data:
 *  \li print a starting header.
 *  \li print the actual record for an `airport*`
 */
static void airport_print_header (unsigned line, bool use_usec)
{
  g_usec_fmt = use_usec > 0.0 ? "%.2f" : "%.2f%%";

  printf ("line: %u:\n"
          "  Rec  ICAO       IATA       cont  location                full_name                                                   lat       lon  %s\n"
          "-----------------------------------------------------------------------------------------------------------------------------------------------\n",
          line, use_usec ? "usec" : "hit-rate");
}

static void airport_print_rec (const char *ICAO, const airport *a, size_t idx, double val)
{
  static const pos_t pos0 = { 0.0, 0.0 };
  char   usec_buf [20];

  const char  *IATA      = a ? a->IATA      : "?";
  const char  *full_name = a ? a->full_name : "?";
  const char  *continent = a ? a->continent : "?";
  const char  *location  = a ? a->location  : "?";
  const pos_t *pos       = a ? &a->pos      : &pos0;

  usec_buf[0] = '-';
  usec_buf[1] = '\0';
  if (val > 0.0)
     snprintf (usec_buf, sizeof(usec_buf), g_usec_fmt, val);

  printf ("%5zu  '%-8.8s' '%-8.8s' '%2.2s'  '%-20.20s'  '%-50.50s'  %9.3f %9.3f  %s\n",
          idx, ICAO, IATA, continent, location, full_name, pos->lat, pos->lon, usec_buf);
}

#if defined(__clang__)
  #pragma clang diagnostic push    "-Winvalid-source-encoding"
  #pragma clang diagnostic ignored "-Winvalid-source-encoding"
#endif

#define ADD_AIRPORT(ICAO, IATA, continent, location, \
                    full_name, lon, lat)            \
        { { ICAO },                                \
          { IATA },                                \
          { continent },                           \
          { location },                            \
          { full_name },                           \
          { lon, lat },                            \
          AIRPORT_CSV,                             \
          NULL                                     \
        }
static const airport airport_tests [] = {
       ADD_AIRPORT ("ENBR", "BGO", "EU", "Bergen",       "Bergen Airport Flesland",                5.218140120, 60.293399810000),
       ADD_AIRPORT ("ENGM", "OSL", "EU", "Oslo",         "Oslo Gardermoen Airport",               11.100399971, 60.193901062012),
       ADD_AIRPORT ("KJFK", "JFK", "NA", "New York",     "John F Kennedy International Airport", -73.778000000, 40.639801000000),
       ADD_AIRPORT ("OTHH", "DOH", "AS", "Doha",         "Hamad International Airport",           51.608050000, 25.273056000000),
       ADD_AIRPORT ("AF10", "URZ", "AS", "OrÅ«zgÄ\\x81n","OrÅ«zgÄ\\x81n Airport",                 66.630897520, 32.9029998779000) // Uruzgan / Afghanistan
     };

#if defined(__clang__)
  #pragma clang diagnostic pop "-Winvalid-source-encoding"
#endif

/**
 * Do some simple tests on the `Modes.airport_list_CSV`.
 */
static void airport_CSV_test_1 (void)
{
  const airport *a;
  const airport *t = airport_tests + 0;
  size_t         i, num_ok;

  printf ("Checking %zu fixed records against \"%s\". ",
          DIM(airport_tests), basename(Modes.airport_db));

  AIRPORT_PRINT_HEADER (false);

  for (i = num_ok = 0; i < DIM(airport_tests); i++, t++)
  {
    a = CSV_lookup_entry (t->ICAO);
    if (a)
       num_ok++;
    airport_print_rec (t->ICAO, a, i, hit_rate);
  }
  printf ("\n"
          "%3zu OKAY\n", num_ok);
  printf ("%3zu FAIL\n", i - num_ok);
}

static void airport_CSV_test_2 (void)
{
  const airport *a;
  size_t         i, num;

  if (!Modes.airport_list_CSV)
     return;

  if (Modes.tests_arg)
       num = Modes.tests_arg;
  else num = 10;

  printf ("\nChecking %zu random records in \"%s\". ", num, basename(Modes.airport_db));
  AIRPORT_PRINT_HEADER (true);

  for (i = 0; i < num; i++)
  {
    const char *ICAO;
    unsigned    rec_num = random_range (0, Modes.airport_num_CSV-1);
    double      usec;

    usec  = get_usec_now();
    ICAO  = (Modes.airport_list_CSV + rec_num) -> ICAO;
    a     = CSV_lookup_entry (ICAO);
    usec  = get_usec_now() - usec;
    airport_print_rec (ICAO, a, rec_num, usec);
  }
  puts ("");
}

/**
 * Do a simple test on the `Modes.airports` created by
 * airports_init_API()`
 */
static void airport_API_test (void)
{
  const airport *a = Modes.airports;
  uint32_t       live, cached;
  uint32_t       num = flight_numbers (&live, &cached);

  printf ("%s():\n", __FUNCTION__);
  printf ("  num: %u, cached: %u, live: %u\n", num, cached, live);

  while (a)
  {
    if (a->type != AIRPORT_CSV)
       printf ("  type: %-6s, full_name: '%s'\n",
               airport_t_str(a->type), a->full_name);
    a = a->next;
  }
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
 * Handling of "Flight Information"
 *
 * A linked list of (live or cached) flight-information.
 */
static flight_info *flight_info_create (const char *callsign, airport_t type)
{
  flight_info *f = calloc (sizeof(*f), 1);

  if (!f)
     return (NULL);

  strncpy (f->callsign, callsign, sizeof(f->callsign)-1);
  f->type    = type;
  f->created = MSEC_TIME();
  return (f);
}

/**
 * Traverse the `g_flight_info` list to get flight-information for this callsign.
 */
flight_info *flight_info_get (const char *callsign)
{
  flight_info *f;

  for (f = g_flight_info; f; f = f->next)
  {
    if (!stricmp(f->callsign, callsign))
       return (f);
  }
  return (NULL);
}

/**
 * Find the flight-info record matching `callsign` or create a new one.
 */
static flight_info *fligh_info_find_or_create (const char *callsign)
{
  flight_info *f = flight_info_get (callsign);

  if (!f)
  {
    f = flight_info_create (callsign, AIRPORT_API_LIVE);
    if (f)
       LIST_ADD_TAIL (flight_info, &g_flight_info, f);
  }
  return (f);
}

/**
 * Exit function for flight-info:
 *  \li Append the `AIRPORT_API_LIVE` records to `Modes.airport_cache`.
 *  \li Free the `g_flight_info` list.
 */
static void flight_info_exit (void)
{
  flight_info *f, *f_next;
  FILE        *fil = fopen (Modes.airport_cache, "at");

  if (!fil)
     LOG_STDERR ("Failed to append to \"%s\": %s\n", Modes.airport_cache, strerror(errno));

  for (f = g_flight_info; f; f = f_next)
  {
    if (fil)
    {
      if (f->type == AIRPORT_API_LIVE)
         fprintf (fil, "%s,%s,%s,%s,%llu\n",
                  airport_t_str(f->type),
                  f->callsign,
                  f->departure,
                  f->destination,
                  f->created);
    }

    f_next = f->next;
    LIST_DELETE (flight_info, &g_flight_info, f);
    free (f);
  }
  if (fil)
    fclose (fil);
  g_flight_info = NULL;
}

static uint32_t flight_numbers (uint32_t *live_p, uint32_t *cached_p)
{
  const flight_info *f;
  uint32_t     num = 0;
  uint32_t     live = 0;
  uint32_t     cached = 0;

  for (f = g_flight_info; f; f = f->next, num++)
  {
    if (f->type == AIRPORT_API_LIVE)
       live++;
    else if (f->type == AIRPORT_API_CACHED)
       cached++;
    else
       TRACE ("Unknown f->type: %d for record %u", f->type, num);
  }

  if (live_p)
     *live_p = live;
  if (cached_p)
     *cached_p = cached;

  return (num);
}

static void flight_info_test (void)
{
  const flight_info *f;
  uint32_t     live, cached;
  uint32_t     num = flight_numbers (&live, &cached);

  printf ("%s():\n", __FUNCTION__);
  printf ("  num: %u, cached: %u, live: %u\n", num, cached, live);

  for (f = g_flight_info; f; f = f->next)
  {
    printf ("  type: %-6s, callsign: '%s', created: %llu, departure: '%s', destination: '%s'\n",
            airport_t_str(f->type), f->callsign, f->created, f->departure, f->destination);
  }
  puts ("");
}
