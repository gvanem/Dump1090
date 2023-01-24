/**
 * \file    aircraft.c
 * \ingroup Main
 * \brief   Handling of aircraft data and ICAO address utilities
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "misc.h"
#include "sqlite3.h"
#include "aircraft.h"

#define USE_VARCHAR 0

#define TRACE(fmt, ...) do {                                                \
                          if (Modes.debug & DEBUG_GENERAL)                  \
                             modeS_flogf (stdout, "%s(%u): " fmt,           \
                                          __FILE__, __LINE__, __VA_ARGS__); \
                        } while (0)

/**
 * \def DB_COLUMNS
 * The Sqlite columns we define.
 */
#define DB_COLUMNS "icao24,reg,manufacturer,callsign"
//                  |      |   |            |
//                  |      |   |            |____ == field 10: "operatorcallsign"
//                  |      |   |_________________ == field 3:  "manufacturername"
//                  |      |_____________________ == field 1:  "registration"
//                  |____________________________ == field 0:  "icao24"

/**
 * \def DB_INSERT
 * The statement used when creating the Sqlite database
 */
#define DB_INSERT  "INSERT INTO aircrafts (" DB_COLUMNS ") VALUES"

static struct sqlite3 *g_db = NULL;

static bool sql_create (void);
static bool sql_open (void);
static bool sql_begin (void);
static bool sql_end (void);
static bool sql_add_entry (uint32_t num, const aircraft_CSV *rec);

static aircraft *aircraft_find (uint32_t addr);
static const     aircraft_CSV *CSV_lookup_entry (uint32_t addr);
static const     aircraft_CSV *sql_lookup_entry (uint32_t addr);

/**
 * Lookup an aircraft in the CSV `Modes.aircraft_list_CSV` or
 * do a SQLite lookup.
 */
static const aircraft_CSV *aircraft_lookup (uint32_t addr, bool *from_sql)
{
  const aircraft     *a;
  const aircraft_CSV *_a;

  if (from_sql)
     *from_sql = false;

  if (Modes.aircraft_list_CSV)
     _a = CSV_lookup_entry (addr);
  else
  {
    a = aircraft_find (addr);
    if (a)
         _a = a->SQL;
    else _a = sql_lookup_entry (addr); /* do the `SELECT * FROM` */
  }
  if (from_sql && _a)
     *from_sql = true;
  return (_a);
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
  aircraft           *a = calloc (sizeof(*a), 1);
  const aircraft_CSV *_a;
  bool                from_sql;

  if (!a)
     return (NULL);

  a->addr       = addr;
  a->seen_first = now;
  a->seen_last  = now;
  a->show       = A_SHOW_FIRST_TIME;
  _a = aircraft_lookup (addr, &from_sql);

  /* We really can't tell if it's unique since we keep no global list of that yet
   */
  Modes.stat.unique_aircrafts++;

  if (!from_sql)
  {
    Modes.stat.unique_aircrafts_CSV++;

    /* This points into the `Modes.aircraft_list_CSV` array.
     * No need to `free()`.
     */
    a->CSV = _a;
  }
  else
  {
    /* Need to duplicate record from sql_exec().
     * free() it on `aircraft_exit(true)`.
     */
    a->SQL = malloc (sizeof(*a->SQL));
    if (a->SQL)
    {
      Modes.stat.unique_aircrafts_SQL++;
      memcpy (a->SQL, _a, sizeof(*a->SQL));
      LOG_FILEONLY ("SQL: %06X, regnum: '%s', callsign: '%s', manufact: '%s', country: '%s'\n",
                    a->addr, a->SQL->reg_num, a->SQL->call_sign, a->SQL->manufact, aircraft_get_country(a->addr));
    }
  }
  return (a);
}

/**
 * Return the aircraft with the specified ICAO address, or NULL if we have
 * no aircraft with this ICAO address.
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
 * Fin the aircraft with address `addr` or create a new one.
 */
aircraft *aircraft_find_or_create (uint32_t addr, uint64_t now)
{
  aircraft *a = aircraft_find (addr);

  if (!a)
  {
    a = aircraft_create (addr, now);
    if (a)
       LIST_ADD_HEAD (aircraft, &Modes.aircrafts, a);
  }
  return (a);
}

/**
 * Return the number of aircrafts we have now.
 */
int aircraft_numbers (void)
{
  aircraft *a = Modes.aircrafts;
  int       num;

  for (num = 0; a; num++)
      a = a->next;
  return (num);
}

/**
 * Add an aircraft record to `Modes.aircraft_list_CSV`.
 */
static int CSV_add_entry (const aircraft_CSV *rec)
{
  static aircraft_CSV *copy = NULL;
  static aircraft_CSV *dest = NULL;
  static aircraft_CSV *hi_end;

  /* Not a valid ICAO address. Parse error?
   */
  if (rec->addr == 0 || rec->addr > 0xFFFFFF)
     return (1);

  if (!copy)
  {
    copy = dest = malloc (ONE_MEGABYTE);  /* initial buffer */
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
  const aircraft_CSV *a = (const aircraft_CSV*) _a;
  const aircraft_CSV *b = (const aircraft_CSV*) _b;

  if (a->addr < b->addr)
     return (-1);
  if (a->addr > b->addr)
     return (1);
  return (0);
}

/**
 * Do a binary search for an aircraft in `Modes.aircraft_list_CSV`.
 */
static const aircraft_CSV *CSV_lookup_entry (uint32_t addr)
{
  aircraft_CSV key = { addr, "" };

  if (!Modes.aircraft_list_CSV)
     return (NULL);
  return bsearch (&key, Modes.aircraft_list_CSV, Modes.aircraft_num_CSV,
                  sizeof(*Modes.aircraft_list_CSV), CSV_compare_on_addr);
}

/**
 * If `Modes.debug != 0`, do a simple test on the `Modes.aircraft_list_CSV`.
 *
 * Also called if `Modes.aircraft_db_sql != 0` to compare the lookup speed
 * of Sqlite3 compared to our `bsearch()` lookup.
 */
static void aircraft_tests (void)
{
  const char  *country;
  unsigned     i, num_ok;
  static const aircraft_CSV a_tests[] = {
               { 0xAA3496, "N757FQ",  "Cessna" },
               { 0x800737, "VT-ANQ",  "Boeing" }, /* callsign: AIRINDIA */
               { 0xAB34DE, "N821DA",  "Beech"  },
               { 0x800737, "VT-ANQ",  "Boeing" },
               { 0xA713D5, "N555UW",  "Piper"  },
               { 0x3532C1, "T.23-01", "AIRBUS" },  /* callsign: AIRMIL, Spain */
             };
  const aircraft_CSV *t = a_tests + 0;
  char  sql_file [MAX_PATH] = "";

  if (Modes.aircraft_sql[0])
     snprintf (sql_file, sizeof(sql_file), "\nand against \"%s\"",
               basename(Modes.aircraft_sql));

  LOG_STDOUT ("Checking 5 fixed records against \"%s\"%s:\n",
              basename(Modes.aircraft_db), sql_file);

  for (i = num_ok = 0; i < DIM(a_tests); i++, t++)
  {
    const aircraft_CSV *a_CSV, *a_SQL;
    const char *call_sign = "?";
    const char *reg_num   = "?";
    const char *manufact  = "?";

    a_CSV = CSV_lookup_entry (t->addr);
    a_SQL = sql_lookup_entry (t->addr);

    if (a_CSV)
    {
      if (a_CSV->call_sign[0])
         call_sign = a_CSV->call_sign;
      if (a_CSV->manufact[0])
         manufact = a_CSV->manufact;
      if (a_CSV->reg_num[0])
      {
        reg_num = a_CSV->reg_num;
        num_ok++;
      }
    }
    else if (a_SQL)
    {
      if (a_SQL->call_sign[0])
         call_sign = a_SQL->call_sign;
      if (a_SQL->manufact[0])
         manufact = a_SQL->manufact;
      if (a_SQL->reg_num[0])
      {
        reg_num = a_SQL->reg_num;
        num_ok++;
      }
    }

    country = aircraft_get_country (t->addr);
    LOG_STDOUT ("  addr: 0x%06X, reg-num: %-8s manufact: %-20s country: %-30s %s\n",
                t->addr, reg_num, manufact, country ? country : "?",
                aircraft_is_military(t->addr) ? "Military" : "");
  }
  LOG_STDOUT ("%3u OKAY\n", num_ok);
  LOG_STDOUT ("%3u FAIL\n", i - num_ok);

  if (!Modes.aircraft_list_CSV)
     return;

  LOG_STDOUT ("\nChecking 5 random records in \"%s\"%s:\n",
              basename(Modes.aircraft_db), sql_file);

  for (i = 0; i < 5; i++)
  {
    const aircraft_CSV *a_CSV, *a_SQL;
    unsigned rec_num = random_range (0, Modes.aircraft_num_CSV-1);
    uint32_t addr;
    double   usec;

    usec  = get_usec_now();
    addr  = (Modes.aircraft_list_CSV + rec_num) -> addr;
    a_CSV = CSV_lookup_entry (addr);
    usec  = get_usec_now() - usec;

    LOG_STDOUT ("  CSV rec: %6u: addr: 0x%06X, reg-num: %-8s manufact: %-20.20s callsign: %-10s %6.0f usec\n",
                rec_num, addr,
                a_CSV->reg_num[0]   ? a_CSV->reg_num   : "-",
                a_CSV->manufact[0]  ? a_CSV->manufact  : "-",
                a_CSV->call_sign[0] ? a_CSV->call_sign : "-",
                usec);

    if (Modes.aircraft_db_sql)
    {
      usec  = get_usec_now();
      a_SQL = sql_lookup_entry (addr);
      usec  = get_usec_now() - usec;

      LOG_STDOUT ("  SQL rec:                         reg-num: %-8s manufact: %-20.20s callsign: %-10s %6.0f usec\n",
                  (a_SQL && a_SQL->reg_num[0])   ? a_SQL->reg_num   : "-",
                  (a_SQL && a_SQL->manufact[0])  ? a_SQL->manufact  : "-",
                  (a_SQL && a_SQL->call_sign[0]) ? a_SQL->call_sign : "-",
                  usec);
    }
  }
}

/*
 * Taken from Dump1090-FA's 'net_io.c':
 */
char *aircraft_json_Dump1090_OL3 (const char *url_path, int *len)
{
  MODES_NOTUSED (url_path);
  MODES_NOTUSED (len);
  return (NULL);
}

/**
 * Check if the aircraft .CSV-database is older than 10 days.
 * If so:
 *  1) download the OpenSky .zip file to `%TEMP%\\aircraft-database-temp.zip`,
 *  2) call `unzip - %TEMP%\\aircraft-database-temp.zip > %TEMP%\\aircraft-database-temp.csv`.
 *  3) copy `%TEMP%\\aircraft-database-temp.csv` over to 'db_file'.
 */
bool aircraft_CSV_update (const char *db_file, const char *url)
{
  struct stat st;
  FILE       *p;
  int         rc;
  const char *comspec, * tmp = getenv ("TEMP");
  bool        force_it = false;
  char        tmp_file [MAX_PATH];
  char        zip_file [MAX_PATH];
  char        unzip_cmd [MAX_PATH+50];

  if (!db_file || !url)
  {
    LOG_STDERR ("Illegal parameters; db_file=%s, url=%s.\n", db_file, url);
    return (false);
  }
  if (!tmp)
  {
    LOG_STDERR ("%%TEMP%% is not defined!\n");
    return (false);
  }

  /* '_popen()' does not set 'errno' on non-existing programs.
   * Hence, use 'system()'; invoke the shell and check for '%errorlevel != 0'.
   */
  comspec = getenv ("COMSPEC");
  if (!comspec)
     comspec = "cmd.exe";

  snprintf (unzip_cmd, sizeof(unzip_cmd), "%s /C unzip.exe -h >NUL 2>NUL", comspec);
  rc = system (unzip_cmd);
  if (rc != 0)
  {
    rc == 2 ? LOG_STDERR ("'unzip.exe' not found on PATH.\n") :
              LOG_STDERR ("Failed to run '%s'.\n", unzip_cmd);
    return (false);
  }

  if (stat(db_file, &st) != 0)
  {
    LOG_STDERR ("\nForce updating '%s' since it does not exist.\n", db_file);
    force_it = true;
  }

  snprintf (zip_file, sizeof(zip_file), "%s\\%s.zip", tmp, AIRCRAFT_DATABASE_TMP);
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
      LOG_STDERR ("\nUpdate of '%s' not needed before %.24s.\n", zip_file, ctime(&when));
      return (true);
    }
  }

  LOG_STDERR ("%supdating '%s' from '%s'\n", force_it ? "Force " : "", zip_file, url);

  if (download_file(zip_file, url) <= 0)
  {
    LOG_STDERR ("Failed to download '%s': '%s'\n", zip_file, Modes.wininet_last_error);
    return (false);
  }

  snprintf (tmp_file,  sizeof(tmp_file), "%s\\%s.csv", tmp, AIRCRAFT_DATABASE_TMP);

  /* '-p  extract files to pipe, no messages'
   */
  snprintf (unzip_cmd, sizeof(unzip_cmd), "unzip.exe -p %s > %s", zip_file, tmp_file);

  p = _popen (unzip_cmd, "r");
  if (!p)
  {
    LOG_STDERR ("Failed to run 'unzip.exe': %s\n", strerror(errno));
    return (false);
  }

  _pclose (p);

  LOG_STDERR ("Copying '%s' -> '%s'\n", tmp_file, db_file);
  CopyFile (tmp_file, db_file, FALSE);
  touch_file (db_file);
  return (true);
}

/**
 * The CSV callback for adding a record to `Modes.aircraft_list_CSV`.
 *
 * \param[in]  ctx   the CSV context structure.
 * \param[in]  value the value for this CSV field in record `ctx->rec_num`.
 *
 * Match the fields 0, 1, 3 and 10 for a record like this:
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
  static aircraft_CSV rec = { 0, "" };
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
 * Initialize the aircraft-database from .csv file.
 *
 * But if the .sqlite file exist, use that instead.
 */
bool aircraft_CSV_load (void)
{
  struct stat st_csv;
  struct stat st_sql;
  double usec;
  double csv_load_t  = 0.0;
  double sql_load_t  = 0.0;
  double sql_create_t = 0.0;
  bool   sql_created = false;
  bool   sql_opened  = false;

  if (!stricmp(Modes.aircraft_db, "NUL"))   /* User want no .csv file */
     return (true);

  if (stat(Modes.aircraft_db, &st_csv) != 0)
  {
    LOG_STDERR ("Aircraft database \"%s\" does not exist.\n", Modes.aircraft_db);
    return (false);
  }

  get_usec_now(); /* calls 'QueryPerformanceFrequency()' */

  if (Modes.aircraft_db_sql)
  {
#ifdef SQLITE_OMIT_AUTOINIT
    int rc = sqlite3_initialize();

    if (rc != SQLITE_OK)
       LOG_STDERR ("Sqlite init failed.\n" );
    else
#endif
    {
      snprintf (Modes.aircraft_sql, sizeof(Modes.aircraft_sql), "%s.sqlite", Modes.aircraft_db);
      if (stat(Modes.aircraft_sql, &st_sql) == 0)
      {
        usec = get_usec_now();
        sql_opened = sql_open();
        sql_load_t = get_usec_now() - usec;

        /**
         * \todo If `sql_st.st_mtime < csv_st.st_mtime`, call `sql_create()`?
         */
      }
      else
      {
        TRACE ("Aircraft Sqlite database \"%s\" does not exist.\n"
               "Creating new from \"%s\".\n", Modes.aircraft_sql, Modes.aircraft_db);
        sql_created = sql_create();
      }
    }
  }

  if (!sql_opened || sql_created ||
      Modes.debug)   /* To compare speed of 'Modes.aircraft_list_CSV' lookup VS. sql_lookup_entry() */
  {
    memset (&Modes.csv_ctx, '\0', sizeof(Modes.csv_ctx));
    Modes.csv_ctx.file_name = Modes.aircraft_db;
    Modes.csv_ctx.delimiter = ',';
    Modes.csv_ctx.callback  = CSV_callback;
    Modes.csv_ctx.line_size = 2000;

    usec = get_usec_now();

    if (!CSV_open_and_parse_file(&Modes.csv_ctx))
    {
      LOG_STDERR ("Parsing of \"%s\" failed: %s\n", Modes.aircraft_db, strerror(errno));
      return (false);
    }

    DEBUG (DEBUG_GENERAL, "Parsed %u records from: \"%s\"\n", Modes.aircraft_num_CSV, Modes.aircraft_db);

    if (Modes.aircraft_num_CSV > 0)
    {
      qsort (Modes.aircraft_list_CSV, Modes.aircraft_num_CSV, sizeof(*Modes.aircraft_list_CSV),
             CSV_compare_on_addr);
      csv_load_t = get_usec_now() - usec;
    }
  }

  if (sql_created && Modes.aircraft_num_CSV > 0)
  {
    const aircraft_CSV *a = Modes.aircraft_list_CSV + 0;
    uint32_t i;

    LOG_STDOUT ("Creating SQL-database... ");
    usec = get_usec_now();
    sql_begin();

    for (i = 0; i < Modes.aircraft_num_CSV; i++, a++)
        sql_add_entry (i, a);

    sql_end();
    sql_create_t = get_usec_now() - usec;
    LOG_STDOUT ("\n");
  }

  if (Modes.debug)
  {
    DEBUG (DEBUG_GENERAL, "CSV loaded and parsed in %.3f ms.\n", csv_load_t/1E3);
    if (sql_create_t > 0.0)
         DEBUG (DEBUG_GENERAL, "SQL created in %.3f ms.\n", sql_create_t/1E3);
    else DEBUG (DEBUG_GENERAL, "SQL loaded in %.3f ms.\n", sql_load_t/1E3);
    aircraft_tests();
   }
  return (true);
}

/**
 * Declare ICAO registration address ranges and country.
 * Mostly generated from the assignment table in the appendix to Chapter 9 of
 * Annex 10 Vol III, Second Edition, July 2007 (with amendments through 88-A, 14/11/2013)
 *
 * Rewritten from `web_root-Tar1090/flags.js`.
 */
static const ICAO_range country_range[] = {
     { 0x004000, 0x0043FF, "Zimbabwe" },
     { 0x006000, 0x006FFF, "Mozambique" },
     { 0x008000, 0x00FFFF, "South Africa" },
     { 0x010000, 0x017FFF, "Egypt" },
     { 0x018000, 0x01FFFF, "Libya" },
     { 0x020000, 0x027FFF, "Morocco" },
     { 0x028000, 0x02FFFF, "Tunisia" },
     { 0x030000, 0x0303FF, "Botswana" },
     { 0x032000, 0x032FFF, "Burundi" },
     { 0x034000, 0x034FFF, "Cameroon" },
     { 0x035000, 0x0353FF, "Comoros" },
     { 0x036000, 0x036FFF, "Congo" },
     { 0x038000, 0x038FFF, "Cote d'Ivoire" },
     { 0x03E000, 0x03EFFF, "Gabon" },
     { 0x040000, 0x040FFF, "Ethiopia" },
     { 0x042000, 0x042FFF, "Equatorial Guinea" },
     { 0x044000, 0x044FFF, "Ghana" },
     { 0x046000, 0x046FFF, "Guinea" },
     { 0x048000, 0x0483FF, "Guinea-Bissau" },
     { 0x04A000, 0x04A3FF, "Lesotho" },
     { 0x04C000, 0x04CFFF, "Kenya" },
     { 0x050000, 0x050FFF, "Liberia" },
     { 0x054000, 0x054FFF, "Madagascar" },
     { 0x058000, 0x058FFF, "Malawi" },
     { 0x05A000, 0x05A3FF, "Maldives" },
     { 0x05C000, 0x05CFFF, "Mali" },
     { 0x05E000, 0x05E3FF, "Mauritania" },
     { 0x060000, 0x0603FF, "Mauritius" },
     { 0x062000, 0x062FFF, "Niger" },
     { 0x064000, 0x064FFF, "Nigeria" },
     { 0x068000, 0x068FFF, "Uganda" },
     { 0x06A000, 0x06A3FF, "Qatar" },
     { 0x06C000, 0x06CFFF, "Central African Republic" },
     { 0x06E000, 0x06EFFF, "Rwanda" },
     { 0x070000, 0x070FFF, "Senegal" },
     { 0x074000, 0x0743FF, "Seychelles" },
     { 0x076000, 0x0763FF, "Sierra Leone" },
     { 0x078000, 0x078FFF, "Somalia" },
     { 0x07A000, 0x07A3FF, "Swaziland" },
     { 0x07C000, 0x07CFFF, "Sudan" },
     { 0x080000, 0x080FFF, "Tanzania" },
     { 0x084000, 0x084FFF, "Chad" },
     { 0x088000, 0x088FFF, "Togo" },
     { 0x08A000, 0x08AFFF, "Zambia" },
     { 0x08C000, 0x08CFFF, "DR Congo" },
     { 0x090000, 0x090FFF, "Angola" },
     { 0x094000, 0x0943FF, "Benin" },
     { 0x096000, 0x0963FF, "Cape Verde" },
     { 0x098000, 0x0983FF, "Djibouti" },
     { 0x09A000, 0x09AFFF, "Gambia" },
     { 0x09C000, 0x09CFFF, "Burkina Faso" },
     { 0x09E000, 0x09E3FF, "Sao Tome and Principe" },
     { 0x0A0000, 0x0A7FFF, "Algeria" },
     { 0x0A8000, 0x0A8FFF, "Bahamas" },
     { 0x0AA000, 0x0AA3FF, "Barbados" },
     { 0x0AB000, 0x0AB3FF, "Belize" },
     { 0x0AC000, 0x0ACFFF, "Colombia" },
     { 0x0AE000, 0x0AEFFF, "Costa Rica" },
     { 0x0B0000, 0x0B0FFF, "Cuba" },
     { 0x0B2000, 0x0B2FFF, "El Salvador" },
     { 0x0B4000, 0x0B4FFF, "Guatemala" },
     { 0x0B6000, 0x0B6FFF, "Guyana" },
     { 0x0B8000, 0x0B8FFF, "Haiti" },
     { 0x0BA000, 0x0BAFFF, "Honduras" },
     { 0x0BC000, 0x0BC3FF, "Saint Vincent and the Grenadines" },
     { 0x0BE000, 0x0BEFFF, "Jamaica" },
     { 0x0C0000, 0x0C0FFF, "Nicaragua" },
     { 0x0C2000, 0x0C2FFF, "Panama" },
     { 0x0C4000, 0x0C4FFF, "Dominican Republic" },
     { 0x0C6000, 0x0C6FFF, "Trinidad and Tobago" },
     { 0x0C8000, 0x0C8FFF, "Suriname" },
     { 0x0CA000, 0x0CA3FF, "Antigua and Barbuda" },
     { 0x0CC000, 0x0CC3FF, "Grenada" },
     { 0x0D0000, 0x0D7FFF, "Mexico" },
     { 0x0D8000, 0x0DFFFF, "Venezuela" },
     { 0x100000, 0x1FFFFF, "Russia" },
     { 0x201000, 0x2013FF, "Namibia" },
     { 0x202000, 0x2023FF, "Eritrea" },
     { 0x300000, 0x33FFFF, "Italy" },
     { 0x340000, 0x37FFFF, "Spain" },
     { 0x380000, 0x3BFFFF, "France" },
     { 0x3C0000, 0x3FFFFF, "Germany" },

     // UK territories are officially part of the UK range.
     // Add extra entries that are above the UK and take precedence
     { 0x400000, 0x4001BF, "Bermuda" },
     { 0x4001C0, 0x4001FF, "Cayman Islands" },
     { 0x400300, 0x4003FF, "Turks and Caicos Islands" },
     { 0x424135, 0x4241F2, "Cayman Islands" },
     { 0x424200, 0x4246FF, "Bermuda" },
     { 0x424700, 0x424899, "Cayman Islands" },
     { 0x424B00, 0x424BFF, "Isle of Man" },
     { 0x43BE00, 0x43BEFF, "Bermuda" },
     { 0x43E700, 0x43EAFD, "Isle of Man" },
     { 0x43EAFE, 0x43EEFF, "Guernsey" },

     // Catch all United Kingdom for the even more obscure stuff
     { 0x400000, 0x43FFFF, "United Kingdom" },
     { 0x440000, 0x447FFF, "Austria" },
     { 0x448000, 0x44FFFF, "Belgium" },
     { 0x450000, 0x457FFF, "Bulgaria" },
     { 0x458000, 0x45FFFF, "Denmark" },
     { 0x460000, 0x467FFF, "Finland" },
     { 0x468000, 0x46FFFF, "Greece" },
     { 0x470000, 0x477FFF, "Hungary" },
     { 0x478000, 0x47FFFF, "Norway" },
     { 0x480000, 0x487FFF, "Netherland" },
     { 0x488000, 0x48FFFF, "Poland" },
     { 0x490000, 0x497FFF, "Portugal" },
     { 0x498000, 0x49FFFF, "Czechia" },
     { 0x4A0000, 0x4A7FFF, "Romania" },
     { 0x4A8000, 0x4AFFFF, "Sweden" },
     { 0x4B0000, 0x4B7FFF, "Switzerland" },
     { 0x4B8000, 0x4BFFFF, "Turkey" },
     { 0x4C0000, 0x4C7FFF, "Serbia" },
     { 0x4C8000, 0x4C83FF, "Cyprus" },
     { 0x4CA000, 0x4CAFFF, "Ireland" },
     { 0x4CC000, 0x4CCFFF, "Iceland" },
     { 0x4D0000, 0x4D03FF, "Luxembourg" },
     { 0x4D2000, 0x4D2FFF, "Malta" },
     { 0x4D4000, 0x4D43FF, "Monaco" },
     { 0x500000, 0x5003FF, "San Marino" },
     { 0x501000, 0x5013FF, "Albania" },
     { 0x501C00, 0x501FFF, "Croatia" },
     { 0x502C00, 0x502FFF, "Latvia" },
     { 0x503C00, 0x503FFF, "Lithuania" },
     { 0x504C00, 0x504FFF, "Moldova" },
     { 0x505C00, 0x505FFF, "Slovakia" },
     { 0x506C00, 0x506FFF, "Slovenia" },
     { 0x507C00, 0x507FFF, "Uzbekistan" },
     { 0x508000, 0x50FFFF, "Ukraine" },
     { 0x510000, 0x5103FF, "Belarus" },
     { 0x511000, 0x5113FF, "Estonia" },
     { 0x512000, 0x5123FF, "Macedonia" },
     { 0x513000, 0x5133FF, "Bosnia and Herzegovina" },
     { 0x514000, 0x5143FF, "Georgia" },
     { 0x515000, 0x5153FF, "Tajikistan" },
     { 0x516000, 0x5163FF, "Montenegro" },
     { 0x600000, 0x6003FF, "Armenia" },
     { 0x600800, 0x600BFF, "Azerbaijan" },
     { 0x601000, 0x6013FF, "Kyrgyzstan" },
     { 0x601800, 0x601BFF, "Turkmenistan" },
     { 0x680000, 0x6803FF, "Bhutan" },
     { 0x681000, 0x6813FF, "Micronesia" },
     { 0x682000, 0x6823FF, "Mongolia" },
     { 0x683000, 0x6833FF, "Kazakhstan" },
     { 0x684000, 0x6843FF, "Palau" },
     { 0x700000, 0x700FFF, "Afghanistan" },
     { 0x702000, 0x702FFF, "Bangladesh" },
     { 0x704000, 0x704FFF, "Myanmar" },
     { 0x706000, 0x706FFF, "Kuwait" },
     { 0x708000, 0x708FFF, "Laos" },
     { 0x70A000, 0x70AFFF, "Nepal" },
     { 0x70C000, 0x70C3FF, "Oman" },
     { 0x70E000, 0x70EFFF, "Cambodia"},
     { 0x710000, 0x717FFF, "Saudi Arabia" },
     { 0x718000, 0x71FFFF, "South Korea" },
     { 0x720000, 0x727FFF, "North Korea" },
     { 0x728000, 0x72FFFF, "Iraq" },
     { 0x730000, 0x737FFF, "Iran" },
     { 0x738000, 0x73FFFF, "Israel" },
     { 0x740000, 0x747FFF, "Jordan" },
     { 0x748000, 0x74FFFF, "Lebanon" },
     { 0x750000, 0x757FFF, "Malaysia" },
     { 0x758000, 0x75FFFF, "Philippines" },
     { 0x760000, 0x767FFF, "Pakistan" },
     { 0x768000, 0x76FFFF, "Singapore" },
     { 0x770000, 0x777FFF, "Sri Lanka" },
     { 0x778000, 0x77FFFF, "Syria" },
     { 0x789000, 0x789FFF, "Hong Kong" },
     { 0x780000, 0x7BFFFF, "China" },
     { 0x7C0000, 0x7FFFFF, "Australia" },
     { 0x800000, 0x83FFFF, "India" },
     { 0x840000, 0x87FFFF, "Japan" },
     { 0x880000, 0x887FFF, "Thailand" },
     { 0x888000, 0x88FFFF, "Viet Nam" },
     { 0x890000, 0x890FFF, "Yemen" },
     { 0x894000, 0x894FFF, "Bahrain" },
     { 0x895000, 0x8953FF, "Brunei" },
     { 0x896000, 0x896FFF, "United Arab Emirates" },
     { 0x897000, 0x8973FF, "Solomon Islands" },
     { 0x898000, 0x898FFF, "Papua New Guinea" },
     { 0x899000, 0x8993FF, "Taiwan" },
     { 0x8A0000, 0x8A7FFF, "Indonesia" },
     { 0x900000, 0x9003FF, "Marshall Islands" },
     { 0x901000, 0x9013FF, "Cook Islands" },
     { 0x902000, 0x9023FF, "Samoa" },
     { 0xA00000, 0xAFFFFF, "United States" },
     { 0xC00000, 0xC3FFFF, "Canada" },
     { 0xC80000, 0xC87FFF, "New Zealand" },
     { 0xC88000, 0xC88FFF, "Fiji" },
     { 0xC8A000, 0xC8A3FF, "Nauru" },
     { 0xC8C000, 0xC8C3FF, "Saint Lucia" },
     { 0xC8D000, 0xC8D3FF, "Tonga" },
     { 0xC8E000, 0xC8E3FF, "Kiribati" },
     { 0xC90000, 0xC903FF, "Vanuatu" },
     { 0xE00000, 0xE3FFFF, "Argentina" },
     { 0xE40000, 0xE7FFFF, "Brazil" },
     { 0xE80000, 0xE80FFF, "Chile" },
     { 0xE84000, 0xE84FFF, "Ecuador" },
     { 0xE88000, 0xE88FFF, "Paraguay" },
     { 0xE8C000, 0xE8CFFF, "Peru" },
     { 0xE90000, 0xE90FFF, "Uruguay" },
     { 0xE94000, 0xE94FFF, "Bolivia" }
   };

const char *aircraft_get_country (uint32_t addr)
{
  const ICAO_range *r = country_range + 0;
  uint16_t          i;

  for (i = 0; i < DIM(country_range); i++, r++)
      if (addr >= r->low && addr <= r->high)
         return (r->country);
  return (NULL);
}

/**
 * Returns TRUE if the ICAO address is in one of these military ranges.
 */
static const ICAO_range military_range[] = {
     { 0xADF7C8,  0xAFFFFF, NULL },
     { 0x010070,  0x01008F, NULL },
     { 0x0A4000,  0x0A4FFF, NULL },
     { 0x33FF00,  0x33FFFF, NULL },
     { 0x350000,  0x37FFFF, NULL },
     { 0x3A8000,  0x3AFFFF, NULL },
     { 0x3B0000,  0x3BFFFF, NULL },
     { 0x3EA000,  0x3EBFFF, NULL },
     { 0x3F4000,  0x3FBFFF, NULL },
     { 0x400000,  0x40003F, NULL },
     { 0x43C000,  0x43CFFF, NULL },
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
     { 0x7D0000,  0x7FFFFF, NULL },
     { 0x800200,  0x8002FF, NULL },
     { 0xC20000,  0xC3FFFF, NULL },
     { 0xE40000,  0xE41FFF, NULL }
   };

bool aircraft_is_military (uint32_t addr)
{
  const ICAO_range *r = military_range + 0;
  uint16_t          i;

  for (i = 0; i < DIM(military_range); i++, r++)
      if (addr >= r->low && addr <= r->high)
         return (true);
  return (false);
}

/**
 * Convert 24-bit big-endian (network order) to host order format.
 */
uint32_t aircraft_get_addr (uint8_t a0, uint8_t a1, uint8_t a2)
{
  return ((a0 << 16) | (a1 << 8) | a2);
}

/**
 * Return the hex-string for the 24-bit ICAO address in `_a[0..2]`.
 * Also look for the registration number and manufacturer from
 * the CSV or SQL data structures.
 */
const char *aircraft_get_details (const uint8_t *_a)
{
  static char         buf[100];
  const aircraft_CSV *a;
  char               *p = buf;
  size_t              sz, left = sizeof(buf);
  uint32_t            addr = aircraft_get_addr (_a[0], _a[1], _a[2]);

  sz = snprintf (p, left, "%06X", addr);
  p    += sz;
  left -= sz;

  a = aircraft_lookup (addr, NULL);
  if (a && a->reg_num[0])
     snprintf (p, left, " (reg-num: %s, manuf: %s, call-sign: %s%s)",
               a->reg_num, a->manufact[0] ? a->manufact : "?", a->call_sign[0] ? a->call_sign : "?",
               aircraft_is_military(addr) ? ", Military" : "");
  return (buf);
}

/**
 * Sqlite3 interface functions
 */
static int sql_callback (void *cb_arg, int argc, char **argv, char **col_name)
{
  aircraft_CSV *a = (aircraft_CSV*) cb_arg;

  if (argc == 4 && mg_unhexn(argv[0], 6) == a->addr)
  {
    strncpy (a->reg_num, argv[1], sizeof(a->reg_num)-1);
    strncpy (a->manufact, argv[2], sizeof(a->manufact)-1);
    strncpy (a->call_sign, argv[3], sizeof(a->call_sign)-1);
  }
  (void) col_name;
  return (0);
}

static const aircraft_CSV *sql_lookup_entry (uint32_t addr)
{
  static aircraft_CSV  a;
  const  aircraft_CSV *ret = NULL;
  char                 query [100];
  char                *err_msg = NULL;
  uint32_t             addr2;
  int                  rc;

  if (!g_db)
     return (NULL);

  memset (&a, '\0', sizeof(a));
  a.addr = addr;
  addr2  = addr;
  snprintf (query, sizeof(query), "SELECT * FROM aircrafts WHERE icao24='%06x';", addr);
  rc = sqlite3_exec (g_db, query, sql_callback, &a, &err_msg);
  Modes.stat.aircrafts_SQL_exec++;

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
  TRACE ("err: %d, %s\n", err, str);
  (void) cb_arg;
}

static bool sql_init (const char *what, int flags)
{
  int rc;

  if (!g_db)
     sqlite3_config (SQLITE_CONFIG_LOG, sql_log, NULL);

  rc = sqlite3_open_v2 (Modes.aircraft_sql, &g_db, flags, NULL);
  if (rc != SQLITE_OK)
  {
    TRACE ("Can't %s database: rc: %d, %s\n", what, rc, sqlite3_errmsg(g_db));
    aircraft_exit (false);
    return (false);
  }
  return (true);
}

/**
 * Create the `Modes.aircraft_sql` database with 4 columns.
 *
 * And make the CSV callback add the records into the `Modes.aircraft_sql` file.
 */
static bool sql_create (void)
{
  char *err_msg = NULL;
  int   rc;

  if (!sql_init("create", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE))
     return (false);

#if USE_VARCHAR
  char  buf [400];
  const aircraft_CSV a = { 0 };

  snprintf (buf, sizeof(buf),
            "CREATE TABLE aircrafts (icao24,"
            " reg VARCHAR(%zu), manufacturer VARCHAR(%zu), callsign VARCHAR(%zu));",
            sizeof(a.reg_num)-1, sizeof(a.manufact)-1, sizeof(a.call_sign)-1);

  rc = sqlite3_exec (g_db, buf, NULL, NULL, &err_msg);

#else
  rc = sqlite3_exec (g_db, "CREATE TABLE aircrafts (" DB_COLUMNS ");",
                     NULL, NULL, &err_msg);
#endif

  if (rc != SQLITE_OK)
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
  return sql_init ("open", SQLITE_OPEN_READONLY /* | SQLITE_OPEN_MEMORY */);
}

static bool sql_begin (void)
{
  char *err_msg = NULL;
  int   rc = sqlite3_exec (g_db, "BEGIN;", NULL, NULL, &err_msg);

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
  int   rc = sqlite3_exec (g_db, "END;", NULL, NULL, &err_msg);

  if (rc != SQLITE_OK)
  {
    TRACE ("rc: %d, %s\n", rc, err_msg);
    sqlite3_free (err_msg);
  }
  return (rc == SQLITE_OK);
}

static bool sql_add_entry (uint32_t num, const aircraft_CSV *rec)
{
  aircraft_CSV copy;
  char   buf [sizeof(copy) + sizeof(DB_INSERT) + 100];
  char  *values, *err_msg = NULL;
  int    rc;
  size_t len;

  memcpy (&copy, rec, sizeof(copy));

  /* Use the '%Q' format to escape some control characters.
   * Another "feature" of Sqlite is that upper-case hex values are
   * turned into lower-case when 'SELECT * FROM' is done!
   */
  len = mg_snprintf (buf, sizeof(buf),
                     DB_INSERT " ('%06x',%Q,%Q,%Q)",
                     copy.addr, copy.reg_num, copy.manufact, copy.call_sign);

  values = buf + sizeof(DB_INSERT) + 1;

  rc = sqlite3_exec (g_db, buf, NULL, NULL, &err_msg);

  if (((num+1) % 1000) == 0)
  {
    if (num >= 100000)
       LOG_STDOUT ("%u\b\b\b\b\b\b", num);
    else if (num >= 10000)
       LOG_STDOUT ("%u\b\b\b\b\b", num);
    else if (num >= 1000)
       LOG_STDOUT ("%u\b\b\b\b", num);
  }

  if (rc != SQLITE_OK)
  {
    TRACE ("\nError at record %u: rc:%d, err_msg: %s\nvalues: '%s'\n", num, rc, err_msg, values);
    sqlite3_free (err_msg);
    return (false);
  }
  return (true);
}


/**
 * Return a malloced JSON description of the active planes.
 * But only those whose latitude and longitude are known.
 *
 * Since various Web-clients expects different elements in this returned
 * JSON array, add those which is approprite for that Web-clients only.
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
 */
char *aircraft_make_json (bool extended_client)
{
  struct timeval tv_now;
  aircraft      *a = Modes.aircrafts;
  int            l, buflen = 1024;        /* The initial buffer is incremented as needed. */
  char          *buf = malloc (buflen);
  char          *p = buf;

  if (!buf)
     return (NULL);

  if (extended_client)
  {
    _gettimeofday (&tv_now, NULL);
    l = snprintf (p, buflen, "{\"now\": %lu.%03lu, \"messages\": %llu, \"aircraft\" : [",
                  tv_now.tv_sec, tv_now.tv_usec/1000, Modes.stat.messages_total);

    p      += l;
    buflen -= l;
  }
  else
  {
    *p++ = '[';
    buflen--;
  }

  while (a)
  {
    int altitude = a->altitude;
    int speed    = a->speed;

    /* Convert units to metric if '--metric' was specified.
     * But an 'extended_client' wants altitude and speed in aeronatical units.
     */
    if (Modes.metric && !extended_client)
    {
      altitude = (int) (double) (a->altitude / 3.2828);
      speed    = (int) (1.852 * (double) a->speed);
    }

    if (VALID_POS(a->position))
    {
      size_t f_len = strlen (a->flight);

      while (a->flight[f_len-1] == ' ')   /* do not send trailing spaces */
         f_len--;

      l = snprintf (p, buflen,
                    "{\"hex\": \"%06X\", \"flight\": \"%.*s\", \"lat\": %f, \"lon\": %f, \"altitude\": %d, \"track\": %d, \"speed\": %d",
                    a->addr, (int)f_len, a->flight, a->position.lat, a->position.lon, altitude, a->heading, speed);
      p      += l;
      buflen -= l;

      if (extended_client)
      {
        l = snprintf (p, buflen, ", \"type\": \"%s\", \"messages\": %u, \"seen\": %lu, \"seen_pos\": %lu",
                      "adsb_icao", a->messages, 2, 1 /* tv_now.tv_sec - a->seen_first/1000 */);
        p      += l;
        buflen -= l;
      }

      strcpy (p, "},\n");
      p      += 3;
      buflen -= 3;

      /* Resize if needed.
       */
      if (buflen < 256)
      {
        int used = p - buf;

        buflen += 1024;    /* Our increment. */
        buf = realloc (buf, used + buflen);
        if (!buf)
           return (NULL);

        p = buf + used;
      }
    }
    a = a->next;
  }

  /* Remove the final comma if any, and close the json array
   */
  if (p[-2] == ',')
     p -= 2;

  *p++ = ']';
  if (extended_client)
     *p++ = '}';
  *p = '\0';
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
       */
      LIST_DELETE (aircraft, &Modes.aircrafts, a);
      free (a->SQL);
      free (a);
    }
  }
}

/**
 * Called to:
 *  \li Close the Sqlite interface.
 *  \li And possibly free memory allocated here (if called from `modeS_exit()`).
 */
void aircraft_exit (bool free_aircrafts)
{
  aircraft *a;
  aircraft *a_next;

  if (g_db)
     sqlite3_close (g_db);
  g_db = NULL;

  if (!free_aircrafts)
     return;

  /* Remove all active aircrafts from the list.
   */
#if 1
  for (a = Modes.aircrafts; a; a = a_next)
  {
    a_next = a->next;
    LIST_DELETE (aircraft, &Modes.aircrafts, a);
    free (a->SQL);
    free (a);
  }

#else
  aircraft *a_prev = NULL;

  for (a = Modes.aircrafts; a; a = a_next)
  {
    a_next = a->next;
    free (a->SQL);
    free (a);
    if (!a_prev)
         Modes.aircrafts = a_next;
    else a_prev->next = a_next;
  }
#endif
}


