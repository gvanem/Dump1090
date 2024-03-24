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
#include "interactive.h"
#include "sqlite3.h"
#include "zip.h"
#include "aircraft.h"

#define SEND_RSSI 1 /* make this an option from `dump1090.cfg`? */

/**
 * The name of Aircraft SQL file is based on the name of `Modes.aircraft_db`.
 */
static mg_file_path aircraft_sql  = { "?" };
static bool         have_sql_file = false;

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

static bool sql_init (const char *what, int flags);
static bool sql_create (void);
static bool sql_open (void);
static bool sql_begin (void);
static bool sql_end (void);
static bool sql_add_entry (uint32_t num, const aircraft_info *rec);

static aircraft *aircraft_find (uint32_t addr);
static const     aircraft_info *CSV_lookup_entry (uint32_t addr);
static const     aircraft_info *SQL_lookup_entry (uint32_t addr);
static bool      is_helicopter_type (const char *type);

/**
 * Lookup an aircraft in the CSV `Modes.aircraft_list_CSV` or
 * do a SQLite lookup.
 */
static const aircraft_info *aircraft_lookup (uint32_t addr, bool *from_sql)
{
  const aircraft_info *ai;

  if (from_sql)
     *from_sql = false;

  if (Modes.aircraft_list_CSV)
     ai = CSV_lookup_entry (addr);
  else
  {
    const aircraft *a = aircraft_find (addr);

    if (a)
         ai = a->SQL;
    else ai = SQL_lookup_entry (addr);   /* do the `SELECT * FROM` */
  }
  if (from_sql && ai)
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
  bool                 from_sql;

  if (!a)
     return (NULL);

  a->addr       = addr;
  a->seen_first = now;
  a->seen_last  = now;
  a->show       = A_SHOW_FIRST_TIME;

  ai = aircraft_lookup (addr, &from_sql);
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

    /* This points into the `Modes.aircraft_list_CSV` array.
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
static aircraft *aircraft_find (uint32_t addr)
{
  aircraft *a = Modes.aircrafts;

  while (a)
  {
    if (a->addr == addr)
       return (a);
    a = a->next;
  }
  return (NULL);
}

/**
 * Find the aircraft with address `addr` or create a new one.
 */
aircraft *aircraft_find_or_create (uint32_t addr, uint64_t now)
{
  aircraft *a = aircraft_find (addr);

  if (!a)
  {
    a = aircraft_create (addr, now);
    if (a)
       LIST_ADD_TAIL (aircraft, &Modes.aircrafts, a);
  }
  return (a);
}

/**
 * Return the number of aircrafts we have now.
 */
int aircraft_numbers (void)
{
  const aircraft *a = Modes.aircrafts;
  int   num;

  for (num = 0; a; num++)
      a = a->next;
  return (num);
}

/**
 * Add an aircraft record to `Modes.aircraft_list_CSV`.
 */
static int CSV_add_entry (const aircraft_info *rec)
{
  static aircraft_info *copy = NULL;
  static aircraft_info *dest = NULL;
  static aircraft_info *hi_end;

  /* Not a valid ICAO address. Parse error?
   */
  if (rec->addr == 0 || rec->addr > 0xFFFFFF)
     return (1);

  if (!copy)
  {
    copy   = dest = malloc (ONE_MEGABYTE);  /* initial buffer */
    hi_end = copy + (ONE_MEGABYTE / sizeof(*rec));
  }
  else if (dest == hi_end - 1)
  {
    size_t new_num = 10000 + Modes.aircraft_num_CSV;

    copy   = realloc (Modes.aircraft_list_CSV, sizeof(*rec) * new_num);
    dest   = copy + Modes.aircraft_num_CSV;
    hi_end = copy + new_num;
  }

  if (!copy)
     return (0);

  Modes.aircraft_list_CSV = copy;
  assert (dest < hi_end);
  memcpy (dest, rec, sizeof(*rec));
  Modes.aircraft_num_CSV++;
  dest = copy + Modes.aircraft_num_CSV;
  return (1);
}

/**
 * The compare function for `qsort()` and `bsearch()`.
 */
static int CSV_compare_on_addr (const void *_a, const void *_b)
{
  const aircraft_info *a = (const aircraft_info*) _a;
  const aircraft_info *b = (const aircraft_info*) _b;

  if (a->addr < b->addr)
     return (-1);
  if (a->addr > b->addr)
     return (1);
  return (0);
}

/**
 * Do a binary search for an aircraft in `Modes.aircraft_list_CSV`.
 */
static const aircraft_info *CSV_lookup_entry (uint32_t addr)
{
  aircraft_info key = { addr, "" };

  if (!Modes.aircraft_list_CSV)
     return (NULL);
  return bsearch (&key, Modes.aircraft_list_CSV, Modes.aircraft_num_CSV,
                  sizeof(*Modes.aircraft_list_CSV), CSV_compare_on_addr);
}

/**
 * Do a simple test on the `Modes.aircraft_list_CSV`.
 *
 * Also, if `have_sql_file == true`, compare the lookup speed
 * of Sqlite3 compared to our `bsearch()` lookup.
 */
static void aircraft_test_1 (void)
{
  const char  *country;
  unsigned     i, num_ok;
  static const aircraft_info a_tests [] = {
               { 0xAA3496, "N757FQ",  "Cessna", "" },
               { 0xAB34DE, "N821DA",  "Beech",  "" },
               { 0x800737, "VT-ANQ",  "Boeing", "" },
               { 0xA713D5, "N555UW",  "Piper",  "" },
               { 0x3532C1, "T.23-01", "AIRBUS", "" },  /* callsign: AIRMIL, Spain */
               { 0x00A78C, "ZS-OYT",  "Bell", "H1P" }  /* test for a Helicopter */
             };
  const aircraft_info *t = a_tests + 0;
  mg_file_path sql_file = "";
  bool         heli_found = false;
  const char  *heli_type  = "?";

  if (have_sql_file)
     snprintf (sql_file, sizeof(sql_file), " and \"%s\"", basename(aircraft_sql));

  LOG_STDOUT ("%s(): Checking %zu fixed records against \"%s\"%s:\n",
              __FUNCTION__, DIM(a_tests), basename(Modes.aircraft_db), sql_file);

  for (i = num_ok = 0; i < DIM(a_tests); i++, t++)
  {
    const aircraft_info *a_CSV, *a_SQL;
    const char *reg_num   = "?";
    const char *manufact  = "?";
    const char *type      = "?";
    const char *type2     = NULL;

    a_CSV = CSV_lookup_entry (t->addr);
    a_SQL = SQL_lookup_entry (t->addr);

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

    if (t->type[0] && aircraft_is_helicopter(t->addr, &type2))
    {
      heli_found = true;
      heli_type  = type2;
    }

    country = aircraft_get_country (t->addr, false);
    LOG_STDOUT ("  addr: 0x%06X, reg-num: %-8s manufact: %-20s type: %-4s country: %-30s %s\n",
                t->addr, reg_num, manufact, type, country ? country : "?",
                aircraft_is_military(t->addr, NULL) ? "Military" : "");
  }
  LOG_STDOUT ("%3u OKAY. heli_found: %d -> %s\n", num_ok, heli_found, heli_type);
  LOG_STDOUT ("%3u FAIL\n", i - num_ok);
}

/**
 * As above, but if `have_sql_file == true`, compare the lookup speed
 * of Sqlite3 compared to our `bsearch()` lookup.
 */
static void aircraft_test_2 (void)
{
  unsigned     i;
  mg_file_path sql_file = "";

  if (have_sql_file)
     snprintf (sql_file, sizeof(sql_file), " and \"%s\"", basename(aircraft_sql));

  LOG_STDOUT ("\n%s(): Checking 5 random records in \"%s\"%s:\n",
              __FUNCTION__, basename(Modes.aircraft_db), sql_file);

  if (!Modes.aircraft_list_CSV)
  {
    LOG_STDOUT ("  cannot do this with an empty 'aircraft_list_CSV'\n");
    return;
  }

  for (i = 0; i < 5; i++)
  {
    const aircraft_info *a_CSV, *a_SQL;
    unsigned rec_num = random_range (0, Modes.aircraft_num_CSV-1);
    uint32_t addr;
    double   usec;

    usec  = get_usec_now();
    addr  = (Modes.aircraft_list_CSV + rec_num) -> addr;
    a_CSV = CSV_lookup_entry (addr);
    usec  = get_usec_now() - usec;

    LOG_STDOUT ("  CSV: %6u: addr: 0x%06X, reg-num: %-8s manufact: %-20.20s callsign: %-10.10s type: %-4s %6.0f usec\n",
                rec_num, addr,
                a_CSV->reg_num[0]   ? a_CSV->reg_num   : "-",
                a_CSV->manufact[0]  ? a_CSV->manufact  : "-",
                a_CSV->call_sign[0] ? a_CSV->call_sign : "-",
                a_CSV->type[0]      ? a_CSV->type      : "-",
                usec);

    if (have_sql_file)
    {
      usec  = get_usec_now();
      a_SQL = SQL_lookup_entry (addr);
      usec  = get_usec_now() - usec;

      LOG_STDOUT ("  SQL:         addr: 0x%06X, reg-num: %-8s manufact: %-20.20s callsign: %-10.10s type: %-4s %6.0f usec\n",
                  addr,
                  (a_SQL && a_SQL->reg_num[0])   ? a_SQL->reg_num   : "-",
                  (a_SQL && a_SQL->manufact[0])  ? a_SQL->manufact  : "-",
                  (a_SQL && a_SQL->call_sign[0]) ? a_SQL->call_sign : "-",
                  (a_SQL && a_SQL->type[0])      ? a_SQL->type      : "-",
                  usec);
    }
  }
}

/**
 * Generate a single json .TXT-file (binary mode) and run
 * `jq.exe < %TEMP%\dump1090\filename > NUL` to verify it.
 */
static void aircraft_dump_json (char *data, const char *filename)
{
  FILE   *f;
  size_t  sz = data ? strlen(data) : 0;
  int     rc;
  char    jq_cmd [20 + sizeof(mg_file_path)];
  mg_file_path tmp_file;

  snprintf (tmp_file, sizeof(tmp_file), "%s\\%s", Modes.tmp_dir, filename);
  printf ("  Dumping %d aircrafts (%zu bytes) to '%s'\n", aircraft_numbers(), sz, tmp_file);

  if (!data)
  {
    printf ("  No 'data'! errno: %d/%s\n\n", errno, strerror(errno));
    return;
  }

  f = fopen (tmp_file, "wb+");
  if (!f)
  {
    printf ("  Creating %s failed; errno: %d/%s\n\n", tmp_file, errno, strerror(errno));
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
       printf ("  File %s failed; errno: %d/%s\n\n", tmp_file, errno, strerror(rc));
  else printf ("  Command '%s' failed; rc: %d.\n\n", jq_cmd, rc);
}

/**
 * Generate some json-files to test the `aircraft_make_json()`
 * function with a large number of aircrafts. The data-content does not matter.
 */
static void aircraft_test_3 (void)
{
  int i, num = 50;

  LOG_STDOUT ("\n%s(): Generate and check JSON-data:\n", __FUNCTION__);

  if (!Modes.home_pos_ok)
  {
    Modes.home_pos.lat = 51.5285578;  /* London */
    Modes.home_pos.lon = -0.2420247;
  }

  /* Create a list of aircrafts with a position around our home-position
   */
  for (i = 0; i < num; i++)
  {
    aircraft *a = aircraft_find_or_create (0x470000 + i, MSEC_TIME());

    if (!a)
       break;

    a->position = Modes.home_pos;
    a->position.lat += random_range2 (-2, 2);
    a->position.lon += random_range2 (-2, 2);
    a->altitude = random_range (0, 10000);
    a->heading  = random_range2 (-180, 180);
    a->messages = 1;
    strcpy (a->call_sign, "test");
  }
  Modes.stat.messages_total = num;

  aircraft_dump_json (aircraft_make_json(false), "json-1.txt");
  aircraft_dump_json (aircraft_make_json(true), "json-2.txt");

  /* Test empty json-data too.
   */
  aircraft_exit (true);
  aircraft_dump_json (aircraft_make_json(false), "json-3.txt");
  aircraft_dump_json (aircraft_make_json(true), "json-4.txt");
}

static bool aircraft_tests (void)
{
  aircraft_test_1();
  aircraft_test_2();
  aircraft_test_3();
  return (true);
}

/**
 * The callback called from `zip_extract()`.
 */
static int extract_callback (const char *file, void *arg)
{
  const char *tmp_file = arg;

  TRACE ("Copying extracted file '%s' to '%s'", file, tmp_file);
  if (!CopyFileA (file, tmp_file, FALSE))
     LOG_STDERR ("CopyFileA (\"%s\", \"%s\") failed: %s\n", file, tmp_file, win_strerror(GetLastError()));
  return (0);
}

/**
 * Check if the aircraft .CSV-database is older than 10 days.
 * If so:
 *  1) download the OpenSky .zip file to `%TEMP%\\dump1090\\aircraft-database-temp.zip`,
 *  2) calls `zip_extract (\"%TEMP%\\dump1090\\aircraft-database-temp.zip\", \"%TEMP%\\dump1090\\aircraft-database-temp.csv\")`.
 *  3) copy `%TEMP%\\dump1090\\aircraft-database-temp.csv` over to 'db_file'.
 *  4) remove `aircraft_sql` to force a rebuild of it.
 */
bool aircraft_CSV_update (const char *db_file, const char *url)
{
  struct stat  st;
  bool         force_it = false;
  mg_file_path tmp_file;
  mg_file_path zip_file;

  if (!db_file || !url)
  {
    LOG_STDERR ("Illegal parameters; db_file=%s, url=%s.\n", db_file, url);
    return (false);
  }

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
    time_t expiry = now - 10 * 24 * 3600;

    if (st.st_mtime > expiry)
    {
      when = now + 10 * 24 * 3600;  /* 10 days into the future */

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

  LOG_STDERR ("Deleting '%s' to force a rebuild in 'aircraft_CSV_load()'\n", aircraft_sql);
  DeleteFileA (aircraft_sql);

  have_sql_file = false;
  return (true);
}

/**
 * The CSV callback for adding a record to `Modes.aircraft_list_CSV`.
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
  static aircraft_info rec = { 0, "" };
  int    rc = 1;

  if (ctx->field_num == 0)        /* "icao24" field */
  {
    rec.addr = mg_unhexn (value, strlen(value));
  }
  else if (ctx->field_num == 1)   /* "registration" field */
  {
    strncpy (rec.reg_num, value, sizeof(rec.reg_num)-1);
  }
  else if (ctx->field_num == 3)   /* "manufacturername" field */
  {
    strncpy (rec.manufact, value, sizeof(rec.manufact)-1);
  }
  else if (ctx->field_num == 8)  /* "icaoaircrafttype" field */
  {
    strncpy (rec.type, value, sizeof(rec.type)-1);
  }
  else if (ctx->field_num == 10)  /* "operatorcallsign" field */
  {
    strncpy (rec.call_sign, value, sizeof(rec.call_sign)-1);
  }
  else if (ctx->field_num == ctx->num_fields - 1)  /* we got the last field */
  {
    rc = CSV_add_entry (&rec);
    memset (&rec, '\0', sizeof(rec));    /* ready for a new record. */
  }
  return (rc);
}

/**
 * Initialize the SQL aircraft-database name.
 */
bool aircraft_SQL_set_name (void)
{
  if (strcmp(Modes.aircraft_db, "NUL"))
  {
    struct stat st;

    memset (&st, '\0', sizeof(st));
    snprintf (aircraft_sql, sizeof(aircraft_sql), "%s.sqlite", Modes.aircraft_db);
    have_sql_file = (stat (aircraft_sql, &st) == 0) && (st.st_size > 8*1024);
    TRACE ("Aircraft Sqlite database \"%s\", size: %ld", aircraft_sql, have_sql_file ? st.st_size : 0);

    if (!have_sql_file)
       DeleteFileA (aircraft_sql);
  }
  else
  {
    have_sql_file = false;
    aircraft_sql [0] = '\0';
  }
  return (have_sql_file);
}

/**
 * Initialize the SQLite interface and aircraft-database from the .csv.sqlite file.
 */
static double aircraft_SQL_load (bool *created, bool *opened, struct stat *st_csv)
{
  struct stat st;
  int    diff_days;
  double usec;

  *created = *opened = false;

  if (sqlite3_initialize() != SQLITE_OK)
  {
    LOG_STDERR ("Sqlite init failed.\n" );
    return (false);
  }

  if (!have_sql_file)
  {
    TRACE ("Aircraft Sqlite database \"%s\" does not exist.\n"
           "Creating new from \"%s\".\n", aircraft_sql, Modes.aircraft_db);
    *created = sql_create();
    if (*created)
       have_sql_file = true;
    return (0.0);
  }

  memset (&st, '\0', sizeof(st));
  stat (aircraft_sql, &st);
  diff_days = (st_csv->st_mtime - st.st_mtime) / (3600*24);

  TRACE ("'%s' is %d days %s than the CSV-file",
         aircraft_sql, abs(diff_days), diff_days < 0 ? "newer" : "older");
  usec = get_usec_now();
  *opened = sql_open();
  return (get_usec_now() - usec);
}

/**
 * Initialize the aircraft-database from .csv file.
 *
 * But if the .sqlite file exist, use that instead.
 */
bool aircraft_CSV_load (void)
{
  static bool done = false;
  struct stat st_csv;
  double usec;
  double csv_load_t   = 0.0;
  double sql_load_t   = 0.0;
  double sql_create_t = 0.0;
  bool   sql_created  = false;
  bool   sql_opened   = false;

  assert (!done);                    /* Do this only once */
  assert (aircraft_sql[0] != '?');   /* Ensure 'aircraft_set_SQL_file()' was called */
  done = true;

  if (!stricmp(Modes.aircraft_db, "NUL"))   /* User want no '.csv' or '.csv.sqlite' file */
     return (true);

  if (stat(Modes.aircraft_db, &st_csv) != 0)
  {
    LOG_STDERR ("Aircraft database \"%s\" does not exist.\n"
                "Do a \"%s --update\" to download and generate it.\n",
                Modes.aircraft_db, Modes.who_am_I);
    return (false);
  }

  sql_load_t = aircraft_SQL_load (&sql_created, &sql_opened, &st_csv);

  if (aircraft_sql[0])
     LOG_STDERR ("%susing Sqlite file: \"%s\".\n", have_sql_file ? "" : "Not ", aircraft_sql);

  /* If `Modes.tests`, open and parse the .CSV-file to compare speed
   * of 'Modes.aircraft_list_CSV' lookup vs. `SQL_lookup_entry()` lookup.
   */
  if (!sql_opened || sql_created || test_contains(Modes.tests, "aircraft"))
  {
    memset (&Modes.csv_ctx, '\0', sizeof(Modes.csv_ctx));
    Modes.csv_ctx.file_name = Modes.aircraft_db;
    Modes.csv_ctx.delimiter = ',';
    Modes.csv_ctx.callback  = CSV_callback;
    Modes.csv_ctx.line_size = 2000;

    usec = get_usec_now();
    LOG_STDOUT ("Loading '%s' could take some time.\n", Modes.aircraft_db);

    if (!CSV_open_and_parse_file(&Modes.csv_ctx))
    {
      LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.aircraft_db, strerror(errno));
      return (false);
    }

    TRACE ("Parsed %u records from: \"%s\"", Modes.aircraft_num_CSV, Modes.aircraft_db);

    if (Modes.aircraft_num_CSV > 0)
    {
      qsort (Modes.aircraft_list_CSV, Modes.aircraft_num_CSV, sizeof(*Modes.aircraft_list_CSV),
             CSV_compare_on_addr);
      csv_load_t = get_usec_now() - usec;
    }
  }

  if (sql_created && Modes.aircraft_num_CSV > 0)
  {
    const aircraft_info *a = Modes.aircraft_list_CSV + 0;
    uint32_t i;

    LOG_STDOUT ("Creating SQL-database '%s'... ", aircraft_sql);
    usec = get_usec_now();
    sql_begin();

    for (i = 0; i < Modes.aircraft_num_CSV; i++, a++)
        if (!sql_add_entry (i, a))
           break;

    sql_end();
    sql_create_t = get_usec_now() - usec;

    if (i != Modes.aircraft_num_CSV)
         LOG_STDOUT ("\nCreated only %u out of %u records!\n", i, Modes.aircraft_num_CSV);
    else LOG_STDOUT ("\nCreated %u records\n", Modes.aircraft_num_CSV);
  }

  if (test_contains(Modes.tests, "aircraft"))
  {
    TRACE ("CSV loaded and parsed in %.3f ms", csv_load_t/1E3);
    if (sql_create_t > 0.0)
         TRACE ("SQL created in %.3f ms", sql_create_t/1E3);
    else TRACE ("SQL loaded in %.3f ms", sql_load_t/1E3);

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
    { 0x004000, 0x0043FF, "ZW", "Zimbabwe" },
    { 0x006000, 0x006FFF, "MZ", "Mozambique" },
    { 0x008000, 0x00FFFF, "ZA", "South Africa" },
    { 0x010000, 0x017FFF, "EG", "Egypt" },
    { 0x018000, 0x01FFFF, "LY", "Libya"  },
    { 0x020000, 0x027FFF, "MA", "Morocco" },
    { 0x028000, 0x02FFFF, "TN", "Tunisia" },
    { 0x030000, 0x0303FF, "BW", "Botswana" },
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
    { 0x048000, 0x0483FF, "GW", "Guinea-Bissau" },
    { 0x04A000, 0x04A3FF, "LS", "Lesotho" },
    { 0x04C000, 0x04CFFF, "KE", "Kenya" },
    { 0x050000, 0x050FFF, "LR", "Liberia" },
    { 0x054000, 0x054FFF, "MG", "Madagascar" },
    { 0x058000, 0x058FFF, "MW", "Malawi" },
    { 0x05A000, 0x05A3FF, "MV", "Maldives" },
    { 0x05C000, 0x05CFFF, "ML", "Mali" },
    { 0x05E000, 0x05E3FF, "MR", "Mauritania" },
    { 0x060000, 0x0603FF, "MU", "Mauritius" },
    { 0x062000, 0x062FFF, "NE", "Niger" },
    { 0x064000, 0x064FFF, "NG", "Nigeria" },
    { 0x068000, 0x068FFF, "UG", "Uganda" },
    { 0x06A000, 0x06A3FF, "QA", "Qatar" },
    { 0x06C000, 0x06CFFF, "CF", "Central African Republic" },
    { 0x06E000, 0x06EFFF, "RW", "Rwanda" },
    { 0x070000, 0x070FFF, "SN", "Senegal" },
    { 0x074000, 0x0743FF, "SC", "Seychelles" },
    { 0x076000, 0x0763FF, "SL", "Sierra Leone" },
    { 0x078000, 0x078FFF, "SO", "Somalia" },
    { 0x07A000, 0x07A3FF, "SZ", "Swaziland" },  // Now Eswatini
    { 0x07C000, 0x07CFFF, "SD", "Sudan" },
    { 0x080000, 0x080FFF, "TZ", "Tanzania" },
    { 0x084000, 0x084FFF, "TD", "Chad" },
    { 0x088000, 0x088FFF, "TG", "Togo" },
    { 0x08A000, 0x08AFFF, "ZM", "Zambia" },
    { 0x08C000, 0x08CFFF, "CD", "DR Congo" },
    { 0x090000, 0x090FFF, "AO", "Angola" },
    { 0x094000, 0x0943FF, "BJ", "Benin" },
    { 0x096000, 0x0963FF, "CV", "Cape Verde" },
    { 0x098000, 0x0983FF, "DJ", "Djibouti" },
    { 0x09A000, 0x09AFFF, "GM", "Gambia" },
    { 0x09C000, 0x09CFFF, "BF", "Burkina Faso" },
    { 0x09E000, 0x09E3FF, "ST", "Sao Tome & Principe" },
    { 0x0A0000, 0x0A7FFF, "DZ", "Algeria" },
    { 0x0A8000, 0x0A8FFF, "BS", "Bahamas" },
    { 0x0AA000, 0x0AA3FF, "BB", "Barbados" },
    { 0x0AB000, 0x0AB3FF, "BZ", "Belize" },
    { 0x0AC000, 0x0ACFFF, "CO", "Colombia" },
    { 0x0AE000, 0x0AEFFF, "CR", "Costa Rica" },
    { 0x0B0000, 0x0B0FFF, "CU", "Cuba" },
    { 0x0B2000, 0x0B2FFF, "SV", "El Salvador" },
    { 0x0B4000, 0x0B4FFF, "GT", "Guatemala" },
    { 0x0B6000, 0x0B6FFF, "GY", "Guyana" },
    { 0x0B8000, 0x0B8FFF, "HT", "Haiti" },
    { 0x0BA000, 0x0BAFFF, "HN", "Honduras" },
    { 0x0BC000, 0x0BC3FF, "VC", "Saint Vincent & the Grenadines" },
    { 0x0BE000, 0x0BEFFF, "JM", "Jamaica" },
    { 0x0C0000, 0x0C0FFF, "NI", "Nicaragua" },
    { 0x0C2000, 0x0C2FFF, "PA", "Panama" },
    { 0x0C4000, 0x0C4FFF, "DO", "Dominican Republic" },
    { 0x0C6000, 0x0C6FFF, "TT", "Trinidad & Tobago" },
    { 0x0C8000, 0x0C8FFF, "SR", "Suriname" },
    { 0x0CA000, 0x0CA3FF, "AG", "Antigua & Barbuda" },
    { 0x0CC000, 0x0CC3FF, "GD", "Grenada" },
    { 0x0D0000, 0x0D7FFF, "MX", "Mexico" },
    { 0x0D8000, 0x0DFFFF, "VE", "Venezuela" },
    { 0x100000, 0x1FFFFF, "RU", "Russia" },
    { 0x201000, 0x2013FF, "NA", "Namibia" },
    { 0x202000, 0x2023FF, "ER", "Eritrea" },
    { 0x300000, 0x33FFFF, "IT", "Italy" },
    { 0x340000, 0x37FFFF, "ES", "Spain" },
    { 0x380000, 0x3BFFFF, "FR", "France" },
    { 0x3C0000, 0x3FFFFF, "DE", "Germany" },

    // UK territories are officially part of the UK range
    // add extra entries that are above the UK and take precedence
    // this is a mess ... let's still try
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

    // catch all United Kingdom for the even more obscure stuff
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
    { 0x498000, 0x49FFFF, "CZ", "Czechia" },  // previously 'Czech Republic'
    { 0x4A0000, 0x4A7FFF, "RO", "Romania" },
    { 0x4A8000, 0x4AFFFF, "SE", "Sweden" },
    { 0x4B0000, 0x4B7FFF, "CH", "Switzerland" },
    { 0x4B8000, 0x4BFFFF, "TR", "Turkey" },
    { 0x4C0000, 0x4C7FFF, "RS", "Serbia" },
    { 0x4C8000, 0x4C83FF, "CY", "Cyprus" },
    { 0x4CA000, 0x4CAFFF, "IE", "Ireland" },
    { 0x4CC000, 0x4CCFFF, "IS", "Iceland" },
    { 0x4D0000, 0x4D03FF, "LU", "Luxembourg" },
    { 0x4D2000, 0x4D2FFF, "MT", "Malta" },
    { 0x4D4000, 0x4D43FF, "MC", "Monaco" },
    { 0x500000, 0x5003FF, "SM", "San Marino" },
    { 0x501000, 0x5013FF, "AL", "Albania" },
    { 0x501C00, 0x501FFF, "HR", "Croatia" },
    { 0x502C00, 0x502FFF, "LV", "Latvia" },
    { 0x503C00, 0x503FFF, "LT", "Lithuania" },
    { 0x504C00, 0x504FFF, "MD", "Moldova"  },
    { 0x505C00, 0x505FFF, "SK", "Slovakia" },
    { 0x506C00, 0x506FFF, "SI", "Slovenia" },
    { 0x507C00, 0x507FFF, "UZ", "Uzbekistan" },
    { 0x508000, 0x50FFFF, "UA", "Ukraine" },
    { 0x510000, 0x5103FF, "BY", "Belarus" },
    { 0x511000, 0x5113FF, "EE", "Estonia" },
    { 0x512000, 0x5123FF, "MK", "Macedonia" },
    { 0x513000, 0x5133FF, "BA", "Bosnia & Herzegovina" },
    { 0x514000, 0x5143FF, "GE", "Georgia" },
    { 0x515000, 0x5153FF, "TJ", "Tajikistan" },
    { 0x516000, 0x5163FF, "ME", "Montenegro" },
    { 0x600000, 0x6003FF, "AM", "Armenia" },
    { 0x600800, 0x600BFF, "AZ", "Azerbaijan" },
    { 0x601000, 0x6013FF, "KG", "Kyrgyzstan" },
    { 0x601800, 0x601BFF, "TM", "Turkmenistan" },
    { 0x680000, 0x6803FF, "BT", "Bhutan" },
    { 0x681000, 0x6813FF, "FM", "Micronesia" },
    { 0x682000, 0x6823FF, "MN", "Mongolia" },
    { 0x683000, 0x6833FF, "KZ", "Kazakhstan" },
    { 0x684000, 0x6843FF, "PW", "Palau" },
    { 0x700000, 0x700FFF, "AF", "Afghanistan" },
    { 0x702000, 0x702FFF, "BD", "Bangladesh" },
    { 0x704000, 0x704FFF, "MM", "Myanmar" },
    { 0x706000, 0x706FFF, "KW", "Kuwait" },
    { 0x708000, 0x708FFF, "LA", "Laos" },
    { 0x70A000, 0x70AFFF, "NP", "Nepal" },
    { 0x70C000, 0x70C3FF, "OM", "Oman" },
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
    { 0x895000, 0x8953FF, "BN", "Brunei" },
    { 0x896000, 0x896FFF, "AE", "United Arab Emirates" },
    { 0x897000, 0x8973FF, "SB", "Solomon Islands" },
    { 0x898000, 0x898FFF, "PG", "Papua New Guinea" },
    { 0x899000, 0x8993FF, "TW", "Taiwan" },
    { 0x8A0000, 0x8A7FFF, "ID", "Indonesia"  },
    { 0x900000, 0x9003FF, "MH", "Marshall Islands" },
    { 0x901000, 0x9013FF, "CK", "Cook Islands" },
    { 0x902000, 0x9023FF, "WS", "Samoa"  },
    { 0xA00000, 0xAFFFFF, "US", "United States" },
    { 0xC00000, 0xC3FFFF, "CA", "Canada" },
    { 0xC80000, 0xC87FFF, "NZ", "New Zealand" },
    { 0xC88000, 0xC88FFF, "FJ", "Fiji" },
    { 0xC8A000, 0xC8A3FF, "NR", "Nauru" },
    { 0xC8C000, 0xC8C3FF, "LC", "Saint Lucia" },
    { 0xC8D000, 0xC8D3FF, "TU", "Tonga" },
    { 0xC8E000, 0xC8E3FF, "KI", "Kiribati" },
    { 0xC90000, 0xC903FF, "VU", "Vanuatu" },
    { 0xE00000, 0xE3FFFF, "AR", "Argentina" },
    { 0xE40000, 0xE7FFFF, "BR", "Brazil" },
    { 0xE80000, 0xE80FFF, "CL", "Chile" },
    { 0xE84000, 0xE84FFF, "EC", "Ecuador" },
    { 0xE88000, 0xE88FFF, "PY", "Paraguay" },
    { 0xE8C000, 0xE8CFFF, "PE", "Peru" },
    { 0xE90000, 0xE90FFF, "UY", "Uruguay" },
    { 0xE94000, 0xE94FFF, "BO", "Bolivia" }
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
     { 0x43C000,  0x43CFFF, "UK" },
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

/**
 * The types of a helicopter (incomplete).
 */
static bool is_helicopter_type (const char *type)
{
  const char *helli_types[] = { "H1P", "H2P", "H1T", "H2T" };
  uint16_t    i;

  if (type[0] != 'H')   /* must start with a 'H' */
     return (false);

  for (i = 0; i < DIM(helli_types); i++)
      if (!stricmp(type, helli_types[i]))
         return (true);
  return (false);
}

/**
 * Figure out if an ICAO-address belongs to a helicopter.
 */
bool aircraft_is_helicopter (uint32_t addr, const char **type_ptr)
{
  const aircraft_info *a;

  if (type_ptr)
     *type_ptr = NULL;

  a = CSV_lookup_entry (addr);
  if (a && is_helicopter_type(a->type))
  {
    if (type_ptr)
       *type_ptr = a->type;
    return (true);
  }
  return (false);
}

/**
 * Convert 24-bit big-endian (network order) to host order format.
 */
uint32_t aircraft_get_addr (uint8_t a0, uint8_t a1, uint8_t a2)
{
  return ((a0 << 16) | (a1 << 8) | a2);
}

const char *aircraft_get_military (uint32_t addr)
{
  static char buf [20];
  const  char *cntry;
  bool   mil = aircraft_is_military (addr, &cntry);
  int    sz;

  if (!mil)
     return ("");

  sz = snprintf (buf, sizeof(buf), "Military");
  if (cntry)
     snprintf (buf+sz, sizeof(buf)-sz, " (%s)", cntry);
  return (buf);
}

/**
 * Return the hex-string for the 24-bit ICAO address in `_a[0..2]`.
 * Also look for the registration number and manufacturer from
 * the CSV or SQL data structures.
 */
const char *aircraft_get_details (const uint8_t *_a)
{
  static char          buf [100];
  const aircraft_info *a;
  char                *p = buf;
  size_t               sz, left = sizeof(buf);
  uint32_t             addr = aircraft_get_addr (_a[0], _a[1], _a[2]);

  sz = snprintf (p, left, "%06X", addr);
  p    += sz;
  left -= sz;

  a = aircraft_lookup (addr, NULL);
  if (a && a->reg_num[0])
     snprintf (p, left, " (reg-num: %s, manuf: %s, call-sign: %s%s)",
               a->reg_num, a->manufact[0] ? a->manufact : "?", a->call_sign[0] ? a->call_sign : "?",
               aircraft_is_military(addr, NULL) ? ", Military" : "");
  return (buf);
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

  if (!Modes.sql_db)
     return (NULL);

  memset (&a, '\0', sizeof(a));
  a.addr = addr;
  addr2  = addr;
  snprintf (query, sizeof(query), "SELECT * FROM aircrafts WHERE icao24='%06x';", addr);
  rc = sqlite3_exec (Modes.sql_db, query, sql_callback, &a, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("SQL error: rc: %d, %s", rc, err_msg);
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
  TRACE ("err: %d, %s", err, str);
  (void) cb_arg;
}

static bool sql_init (const char *what, int flags)
{
  int rc;

  if (!Modes.sql_db) /* 1st time init */
     sqlite3_config (SQLITE_CONFIG_LOG, sql_log, NULL);

  if (!strcmp(what, "load"))
     return (true);

  rc = sqlite3_open_v2 (aircraft_sql, &Modes.sql_db, flags, NULL);
  if (rc != SQLITE_OK)
  {
    TRACE ("Can't %s database: rc: %d, %s", what, rc, sqlite3_errmsg(Modes.sql_db));
    aircraft_exit (false);
    return (false);
  }
  return (true);
}

/**
 * Create the `aircraft_sql` database with 5 columns.
 *
 * And make the CSV callback add the records into the `aircraft_sql` file.
 */
static bool sql_create (void)
{
  char *err_msg = NULL;
  int   rc;

  if (!sql_init("create", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
     return (false);

  rc = sqlite3_exec (Modes.sql_db, "CREATE TABLE aircrafts (" DB_COLUMNS ");",
                     NULL, NULL, &err_msg);

  if (rc != SQLITE_OK &&
      strcmp(err_msg, "table aircrafts already exists")) /* ignore this "error" */
  {
    TRACE ("rc: %d, %s", rc, err_msg);
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
  int   rc = sqlite3_exec (Modes.sql_db, "BEGIN;", NULL, NULL, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("rc: %d, %s", rc, err_msg);
    sqlite3_free (err_msg);
  }
  return (rc == SQLITE_OK);
}

static bool sql_end (void)
{
  char *err_msg = NULL;
  int   rc = sqlite3_exec (Modes.sql_db, "END;", NULL, NULL, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("rc: %d, %s", rc, err_msg);
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
static bool sql_add_entry (uint32_t num, const aircraft_info *rec)
{
  char   buf [sizeof(*rec) + sizeof(DB_INSERT) + 100];
  char  *values, *err_msg = NULL;
  int    rc;

  mg_snprintf (buf, sizeof(buf),
               DB_INSERT " ('%06x',%m,%m,%m,%m)",
               rec->addr,
               mg_print_esc, 0, rec->reg_num,
               mg_print_esc, 0, rec->manufact,
               mg_print_esc, 0, rec->type,
               mg_print_esc, 0, rec->call_sign);

  values = buf + sizeof(DB_INSERT) + 1;

  rc = sqlite3_exec (Modes.sql_db, buf, NULL, NULL, &err_msg);

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
    TRACE ("\nError at record %u: rc:%d, err_msg: %s\nvalues: '%s'", num, rc, err_msg, values);
    sqlite3_free (err_msg);
    return (false);
  }
  return (true);
}

#if SEND_RSSI
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
#endif

/**
 * Fill the JSON buffer `p` for one aircraft.
 */
static size_t aircraft_make_1_json (const aircraft *a, bool extended_client, char *p, int left)
{
  size_t sz;
  char  *p_start = p;
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

  sz = mg_snprintf (p, left,
                    "{\"hex\": \"%06X\", \"flight\": \"%s\", \"lat\": %f, \"lon\": %f, \"altitude\": %d, \"track\": %d, \"speed\": %d",
                     a->addr,
                     a->call_sign,
                     a->position.lat,
                     a->position.lon,
                     altitude,
                     a->heading,
                     speed);

  p    += sz;
  left -= (int)sz;
  assert (left > 100);

  if (extended_client)
  {
    sz = mg_snprintf (p, left,
                      ", \"type\": \"adsb_icao\", \"messages\": %u, \"seen\": %lu, \"seen_pos\": %lu",
                      a->messages, 2, 1 /* tv_now.tv_sec - a->seen_first/1000 */);
    p    += sz;
    left -= (int)sz;
#if SEND_RSSI
    sz = mg_snprintf (p, left, ", \"rssi\": %.1lf", get_signal(a));
    p    += sz;
    left -= (int)sz;
#endif
  }

  assert (left > 3);
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
 * \ref https://github.com/wiedehopf/readsb/blob/dev/README-json.md
 */
char *aircraft_make_json (bool extended_client)
{
  static uint32_t json_file_num = 0;
  struct timeval tv_now;
  aircraft      *a = Modes.aircrafts;
  int            size, left = 1024;    /* The initial buffer is incremented as needed */
  uint32_t       aircrafts = 0;
  char          *buf = malloc (left);
  char          *p = buf;

  if (!buf)
     return (NULL);

  if (extended_client)
  {
    _gettimeofday (&tv_now, NULL);
    size = mg_snprintf (p, left,
                        "{\"now\":%lu.%03lu, \"messages\":%llu, \"aircraft\":\n",
                        tv_now.tv_sec, tv_now.tv_usec/1000, Modes.stat.messages_total);
    p    += size;
    left -= size;
  }

  *p++ = '[';   /* Start the json array */
  left--;

  for (a = Modes.aircrafts; a; a = a->next)
  {
    if (!VALID_POS(a->position))
       continue;

    size  = aircraft_make_1_json (a, extended_client, p, left);
    p    += size;
    left -= size;
    assert (left > 0);
    aircrafts++;

    if (left < 256)    /* Resize 'buf' if needed */
    {
      int used = p - buf;

      left += 1024;    /* Our increment */
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

  assert (left > 3);

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
  return (buf);
}

/**
 * Called from `background_tasks()` 4 times per second.
 *
 * If we don't receive new nessages within `Modes.interactive_ttl`
 * milli-seconds, we remove the aircraft from the list.
 */
void aircraft_remove_stale (uint64_t now)
{
  aircraft *a, *a_next;

  for (a = Modes.aircrafts; a; a = a_next)
  {
    int64_t diff = (int64_t) (now - a->seen_last);

    a_next = a->next;

    /* Mark this plane for a "last time" view on next refresh?
     */
    if (a->show == A_SHOW_NORMAL && diff >= Modes.interactive_ttl - 1000)
    {
      a->show = A_SHOW_LAST_TIME;
    }
    else if (diff > Modes.interactive_ttl)
    {
      /* Remove the element from the linked list.
       * \todo
       * Perhaps copy it to a `aircraft_may_reenter` list?
       * Or leave it in the list with show-state as `A_SHOW_NONE`.
       */
      LIST_DELETE (aircraft, &Modes.aircrafts, a);
      free (a->SQL);
      free (a);
    }
  }
}

/**
 * Set this aircraft's estimated distance to our home position.
 *
 * Assuming a constant good last heading and speed, calculate the
 * new position from that using the elapsed time.
 */
void aircraft_set_est_home_distance (aircraft *a, uint64_t now)
{
  double      heading, distance, gc_distance, cart_distance;
  double      delta_X, delta_Y;
  cartesian_t cpos = { 0.0, 0.0, 0.0 };
  pos_t       epos;
  uint32_t    speed = round ((double)a->speed * 1.852);  /* Km/h */

  if (!Modes.home_pos_ok || speed == 0 || !a->heading_is_valid)
     return;

  if (!VALID_POS(a->EST_position) || a->EST_seen_last < a->seen_last)
     return;

  ASSERT_POS (a->EST_position);

  /* If some issue with speed changing too fast,
   * use the last good speed.
   */
  if (a->speed_last && abs((int)speed - (int)a->speed_last) > 20)
     speed = a->speed_last;

  epos = a->EST_position;
  spherical_to_cartesian (&epos, &cpos);

  /* Ensure heading is in range '[-Phi .. +Phi]'
   */
  if (a->heading >= 180)
       heading = a->heading - 360;
  else heading = a->heading;

  heading = (TWO_PI * heading) / 360;  /* In radians */

  /* knots (1852 m/s) to distance (in meters) traveled in dT msec:
   */
  distance = 0.001852 * (double)speed * (now - a->EST_seen_last);

  delta_X = distance * sin (heading);
  delta_Y = distance * cos (heading);
  cpos.c_x += delta_X;
  cpos.c_y += delta_Y;

  if (!cartesian_to_spherical(&cpos, &epos, heading))
  {
    LOG_FILEONLY ("addr %04X: Invalid epos: %+7.03f lon, %+8.03f lat from heading: %+7.1lf. delta_X: %+8.3lf, delta_Y: %+8.3lf.\n",
                  a->addr, epos.lon, epos.lat, 360.0*heading/TWO_PI, delta_X, delta_Y);
    return;
  }

  a->EST_seen_last = now;
  a->EST_position  = epos;
  ASSERT_POS (a->EST_position);

  gc_distance     = great_circle_dist (a->EST_position, Modes.home_pos);
  cart_distance   = cartesian_distance (&cpos, &Modes.home_pos_cart);
  a->EST_distance = closest_to (a->EST_distance, gc_distance, cart_distance);

#if 0
  LOG_FILEONLY ("addr %04X: heading: %+7.1lf, delta_X: %+8.3lf, delta_Y: %+8.3lf, gc_distance: %6.1lf, cart_distance: %6.1lf\n",
                a->addr, 360.0*heading/TWO_PI, delta_X, delta_Y, gc_distance/1000, cart_distance/1000);
#endif
}

/**
 * Called from show_statistics() to print statistics collected here.
 */
void aircraft_show_stats (void)
{
  LOG_STDOUT ("Aircrafts statistics:\n");
  interactive_clreol();

  LOG_STDOUT (" %8llu unique aircrafts of which %llu was in CSV-file and %llu in SQL-file.\n",
              Modes.stat.unique_aircrafts, Modes.stat.unique_aircrafts_CSV, Modes.stat.unique_aircrafts_SQL);

#if 0  /**\todo print details on unique aircrafts */
  const ac_unique *a;
  char  comment [100];
  size_t  num = 0;

  for (a = g_ac_unique; a; a = a->next, num++)
  {

  }
#endif
}

/**
 * Called to:
 *  \li Close the Sqlite interface.
 *  \li And possibly free memory allocated here (if called from `modeS_exit()`
 *      with `free_aircrafts == true`).
 */
void aircraft_exit (bool free_aircrafts)
{
  aircraft *a, *a_next;

  if (Modes.sql_db)
     sqlite3_close (Modes.sql_db);
  Modes.sql_db = NULL;

  if (!free_aircrafts)
     return;

  /* Remove all active aircrafts from the list.
   */
  for (a = Modes.aircrafts; a; a = a_next)
  {
    a_next = a->next;
    LIST_DELETE (aircraft, &Modes.aircrafts, a);
    free (a->SQL);
    free (a);
  }

  if (Modes.aircraft_list_CSV)
     free (Modes.aircraft_list_CSV);
  Modes.aircraft_list_CSV = NULL;
}
