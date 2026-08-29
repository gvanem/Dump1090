/**\file    adsb-fi.c
 * \ingroup Samplers
 * \brief   Fetches live aircraft data from the adsb.fi opendata API.
 */
#include "misc.h"
#include "smartlist.h"
#include "aircraft.h"
#include "adsb-fi.h"

/**
 * \def MIN_DISTANCE
 * Arbitrary minimum distance (in miles)
 */
#define MIN_DISTANCE 10

/**
 * \def MAX_DISTANCE
 * The max distance (in miles) we are allowed to query for.
 */
#define MAX_DISTANCE 250

/**
 * \def MIN_SLEEP_TIME
 * The minimum Sleep() time between requests to "https://opendata.adsb.fi"
 */
#define MIN_SLEEP_TIME 1000

/**
 * \def POLL_TIME
 * The time between calls to`adsb_fi_print_raw()`.
 */
#define POLL_TIME 1000

/**
 * \def ADSB_FI_URL_FORMAT
 * The URL format to use. When formatted it looks like:
 *   https://opendata.adsb.fi/api/v3/lat/60.542345/lon/-5.00233/dist/150
 *
 * \ref https://github.com/adsbfi/opendata
 */
#define ADSB_FI_URL_FORMAT  "https://opendata.adsb.fi/api/v3/lat/%.6f/lon/%.6f/dist/%u"

/**
 * \typedef ac_element
 * Raw-data we add to `g_data.ac_list`
 */
typedef struct ac_element {
        uint32_t  addr;
        uint32_t  msg_num;
        char      raw_buf [300];
      } ac_element;

/**
 * \typedef mm_element
 * Message-data we add to `g_data.mm_list`
 */
typedef struct mm_element {
        modeS_message      mm;
        struct mm_element *next;
      } mm_element;

/**
 * \typedef adsb_fi_stats
 * Statistics for this module.
 */
typedef struct adsb_fi_stats {
        uint32_t  total_last;      /**< total "ac" elements in JSON-data received last time */
        uint32_t  total_recv;      /**< total JSON-messages received */
        uint64_t  bytes_sent;      /**< total request bytes sent */
        uint64_t  bytes_recv;      /**< total response bytes received */
        uint32_t  requests_sent;   /**< total requests sent */
        uint32_t  responses_recv;  /**< total responses received */
        uint32_t  http_err;        /**< total HTTP != 200 */
        uint32_t  wininet_err;     /**< total WinInet problems */
        uint32_t  ac_missing;
        uint32_t  ac_empty;

      } adsb_fi_stats;

/**
 * \typedef adsb_fi_priv
 *
 * Private data for this module.
 */
typedef struct adsb_fi_priv {
        uint32_t      max_distance;     /**< the max distance (in miles) we will query for */
        bool          max_distance_set;
        uint32_t      sleep_time;       /**< time between requests. In milli-sec */
        bool          sleep_time_set;
        adsb_fi_stats stats;            /**< accumulated statistics */
        char         *url;              /**< the URL we send to */

        smartlist_t  *ac_list;          /**< list of `ac_element` */
        mm_element   *mm_list;          /**< list of `mm_element` */
     } adsb_fi_priv;

static adsb_fi_priv g_data;

static char *send_request (void);
static bool  process_response (mg_str body);
static void  json_dump (const char *data);

/*
 * small test for order of printf()s:
 * With 'cl' it prints "world\nhello\n"
 * With 'clang-cl' it prints "hello\nworld\n"
 * go figure!?
 */
static void print_order (void)
{
#if 1
  int len = printf ("%s\n", "hello") + printf ("%s\n", "world");
  (void) len;
#endif
}

/**
 * Set default values unless set from config-callbacks.
 * Allocate a smartlist for responses.
 */
bool adsb_fi_init (void)
{
  /**
   * `download_to_buf()` to not very thread-safe. Hence do not close
   * WinInet after `download_to_buf()`. But initialize and keep it open
   * from now on. With a large max-distance, we'll get a large number of
   * `AIRPORT_API_PENDING` records in `API_thread_worker()` for
   * "--interactive" mode.
   */
  download_init_once();

  if (!g_data.max_distance_set)
     g_data.max_distance = MAX_DISTANCE;

  if (!g_data.sleep_time_set)
     g_data.sleep_time = MIN_SLEEP_TIME;

  TRACE ("max_distance: %u miles, sleep_time: %u msec\n",
         g_data.max_distance, g_data.sleep_time);

  print_order();

  g_data.ac_list = smartlist_new();
  return (g_data.ac_list != NULL);
}

void adsb_fi_exit (void)
{
  mm_element *mm, *mm_next;

  if (g_data.ac_list)
     smartlist_wipe (g_data.ac_list, free);
  g_data.ac_list = NULL;
  FREE (g_data.url);

  /* free all queued `g_data.mm_list` elements
   */
  for (mm = g_data.mm_list; mm; mm = mm_next)
  {
    mm_next = mm->next;
    LIST_DELETE (mm_element, &g_data.mm_list, mm);
    free (mm);
  }
}

/**
 * Return length of `g_data.ac_list`.
 */
uint32_t adsb_fi_ac_len (void)
{
  return (g_data.ac_list ? smartlist_len (g_data.ac_list) : 0);
}

/**
 * Config-callback for "adsb-fi-max-distance = x".
 */
bool adsb_fi_max_distance (const char *arg)
{
  char    *end;
  uint64_t val = strtoull (arg, &end, 10);

  if (end > arg && val >= MIN_DISTANCE && val <= MAX_DISTANCE)
  {
    g_data.max_distance = (uint32_t) val;
    g_data.max_distance_set = true;
  }
  else
  {
    cfg_illegal_val ("adsb-fi-max-distance", arg);
    LOG_FILEONLY ("Illegal adsb-fi-max-distance: %s miles\n", arg);
  }
  return (true);
}

/**
 * Return max plane distane we query for.
 * In km if `Modes.mteric == true`.
 */
uint32_t adsb_fi_get_max_distance (void)
{
  if (Modes.metric)
     return (uint32_t) round ((double)g_data.max_distance * 1.852);
  return (g_data.max_distance);
}

/**
 * Config-callback for "adsb-fi-sleep-time = x".
 */
bool adsb_fi_sleep_time (const char *arg)
{
  char    *end;
  uint64_t val = strtoull (arg, &end, 10);

  if (end > arg && val >= MIN_SLEEP_TIME)
  {
    g_data.sleep_time = (uint32_t) val;
    g_data.sleep_time_set = true;
  }
  else
  {
    cfg_illegal_val ("adsb-fi-sleep-time", arg);
    LOG_FILEONLY ("Illegal adsb-fi-sleep_time: %s\n", arg);
  }
  return (true);
}

/**
 * Called from `data_thread_fn()` to request JSON-data from `g_data.url`.
 *
 * Process JSON-array response and add uniqueue ICAO addresses to `g_data.ac_list`.
 *
 * Then `adsb_fi_poll()` is called from the main-thread (`background_tasks()`)
 * to print this list.
 */
void adsb_fi_read_loop (void)
{
  while (!Modes.exit)
  {
    /* Check for a good `Modes.home_pos` before sending a request.
     */
    if (!Modes.home_pos_ok || (Modes.home_pos.lat == -1.0 && Modes.home_pos.lon == -1.0))
    {
      FREE (g_data.url);
      Sleep (g_data.sleep_time);
      continue;
    }

    if (!g_data.url)
    {
      /* This assumes our home-pos never changes.
       */
      g_data.url = mg_mprintf (ADSB_FI_URL_FORMAT, Modes.home_pos.lat, Modes.home_pos.lon, g_data.max_distance);
      TRACE ("Fetching from '%s'\n", g_data.url);
    }

    char *data = send_request();
    if (data)
    {
      process_response (mg_str(data));
      g_data.stats.total_recv++;
      json_dump (data);
      free (data);
    }

    /* Call `Sleep(sleep_time)` (minimum 1 sec) between requests.
     * Otherwise we will be blacklisted :-(
     */
    Sleep (g_data.sleep_time);
  }
}

/**
 * In non-interactive mode and w/o the `--only-addr` option,
 * print the raw plane information collected.
 */
static void adsb_fi_print_raw (void)
{
  int   i, len, max = adsb_fi_ac_len();

  printf ("Aircrafts in range: %u, max-dist: %u %s\n",
          g_data.stats.total_last, adsb_fi_get_max_distance(), Modes.metric ? "km" : "Nm");

  len = printf ("%-8s %-9s %8s %6s %9s %9s  %7s",
                "ICAO", "Flight", "Alt", "GS", "Lat", "Lon",
                Modes.metric ? "Km" : "Nm");

  len += printf ("   %4s    %4s   %s\n",
                  "Hdg", "RSSI", "Num") - 13;

  puts ("------------------------------------------"
        "------------------------------------------");

  for (i = 0; i < max; i++)
  {
    const ac_element *ac = smartlist_get (g_data.ac_list, i);
    uint32_t addr = ac->addr;

    if (addr & MODES_NON_ICAO_ADDRESS)
         printf ("~%06X  %-*s %-5u\n", (addr & ~MODES_NON_ICAO_ADDRESS), len, ac->raw_buf, ac->msg_num);
    else printf ("%06X   %-*s %-5u\n", addr, len, ac->raw_buf, ac->msg_num);
  }
  puts ("");
}

/**
 * Called from `background_tasks()` and:
 *   \li call `modeS_user_message()` to process all `g_data.mm_list` messages
 *       in queue.
 *   \li In non-interactive mode and without `--only-addr`, print all
 *       `ac->raw_buf` elements once per `POLL_TIME` (1 sec).
 */
void adsb_fi_poll (void)
{
  mm_element *mm, *mm_next;
  int num = 0;

  /* Pass on all queued `g_data.mm_list` elements for further processing.
   */
  for (mm = g_data.mm_list; mm; mm = mm_next, num++)
  {
    mm_next = mm->next;
    modeS_user_message (&mm->mm);
    LIST_DELETE (mm_element, &g_data.mm_list, mm);
    free (mm);
  }

  /* Do not call `adsb_fi_print_raw()` interactive-mode or with "--only-addr"
   */
  if (Modes.interactive || Modes.only_addr)
     return;

  static uint64_t last_update_ms;
  uint64_t now = MSEC_TIME();

  /* poll-time expired?
   */
  if (last_update_ms == 0 || (now - last_update_ms) >= POLL_TIME)
  {
    adsb_fi_print_raw();
    last_update_ms = now;
  }
}

/**
 * Called from show_statistics() to print statistics collected here.
 */
void adsb_fi_show_stats (void)
{
  LOG_STDOUT ("! \n");
  LOG_STDOUT ("ADSB.FI statistics:\n");
  LOG_STDOUT (" %9llu bytes, %u requests sent.\n", g_data.stats.bytes_sent, g_data.stats.requests_sent);
  LOG_STDOUT (" %9llu bytes, %u responses received.\n", g_data.stats.bytes_recv, g_data.stats.responses_recv);
  LOG_STDOUT (" %9u HTTP errors.\n", g_data.stats.http_err);
}

/**
 * mg_json helpers
 *
 * `mg_json_*()` functions take a `mg_str` span of raw JSON text and a
 * JSONPath-like query string ("$.field"). Here 'ac' is the raw text
 * span of a single aircraft object pulled out of the top-level "$.ac"
 * array, so every query below is relative to that object ("$.xxx").
 *
 * Copies a string field into 'out'; falls back to '"-"' if the
 * field is missing or isn't a string. `mg_json_get_str()` heap-allocates
 * its result, so we free it after copying.
 */
static void get_string (mg_str ac, const char *path, char *out, size_t out_sz)
{
  char *str = mg_json_get_str (ac, path);

  if (str)
  {
    str_lcpy (out, str, out_sz);
    mg_free (str);
  }
  else
    str_lcpy (out, "-", out_sz);
}

/**
 * Returns a numeric field, or 'fallback' if missing / not a number.
 */
static double get_number (struct mg_str ac, const char *path, double fallback)
{
  double val;

  if (mg_json_get_num(ac, path, &val))
     return (val);
  return (fallback);
}

/**
 * "alt_baro" is usually a number, but the API reports "ground" for aircraft
 * that are on the ground, so both number and "ground" must be handled.
 */
static double get_alt (mg_str ac, const char *path, int fallback)
{
  mg_str tok = mg_json_get_tok (ac, path);
  char  *end, val [20];

  if (!tok.buf)
     return (fallback);

  if (tok.buf[0] == '"')     /* value is "ground" */
     return (0.0);

  str_lcpy (val, tok.buf, tok.len + 1);
  return strtod (val, &end);
}

/**
 * Compare on ICAO-address (which is unsigned 24 bit).
 */
static int compare_on_icao (const void **_a, const void **_b)
{
  const ac_element *a = *_a;
  const ac_element *b = *_b;

  if (a->addr < b->addr)
     return (-1);
  if (a->addr > b->addr)
     return (1);
  return (0);
}

static ac_element *ac_list_find (uint32_t addr)
{
  int i, max = adsb_fi_ac_len();

  for (i = 0; i < max; i++)
  {
    ac_element *ac = smartlist_get (g_data.ac_list, i);
    if (ac->addr == addr)
       return (ac);
  }
  return (NULL);
}

/**
 * Find an existing element for `addr` or create a new one.
 */
static ac_element *ac_list_find_or_create (uint32_t addr)
{
  ac_element *ac = ac_list_find (addr);

  if (!ac)
  {
    ac = calloc (sizeof(*ac), 1);
    if (ac)
    {
      ac->addr = addr;
      smartlist_add (g_data.ac_list, ac);
      smartlist_sort (g_data.ac_list, compare_on_icao, 0);
    }
  }
  return (ac);
}

/**
 * Parse received JSON data and add or modify elements to `g_data.ac_list`.
 */
static bool process_response (mg_str body)
{
  int    off;
  mg_str elem;
  mg_str ac_array = mg_json_get_tok (body, "$.ac");

  if (!ac_array.buf || ac_array.buf[0] != '[')
  {
    LOG_FILEONLY ("No 'ac' array found in 'body'?!\n");
    g_data.stats.ac_missing++;
    return (false);
  }

  g_data.stats.total_last = mg_json_get_long (body, "$.total", 0);
  if (g_data.stats.total_last == 0)
  {
    LOG_FILEONLY ("Empty 'ac' array 'body'?!\n");
    g_data.stats.ac_empty++;
    return (false);
  }

  off = 0;
  while ((off = mg_json_next(ac_array, off, NULL, &elem)) != 0)
  {
    ac_element *ac;
    uint32_t    icao_addr;
    char        icao_str [16];
    char        flight_str [16];
    char        category_str [10];
    char        squawk_str [10];
    char        alt_str [16];
    char        gs_str  [16];
    char        dst_str [16];

    get_string (elem, "$.hex", icao_str, sizeof(icao_str));
    get_string (elem, "$.flight", flight_str, sizeof(flight_str));
    get_string (elem, "$.category", category_str, sizeof(category_str));
    get_string (elem, "$.squawk", squawk_str, sizeof(squawk_str));

    double alt   = get_alt    (elem, "$.alt_baro", INVALID_ALTITUDE);
    double vrate = get_number (elem, "$.baro_rate", 0.0);  /* correct field? */
    double gs    = get_number (elem, "$.gs", -1.0);
    double lat   = get_number (elem, "$.lat", 0.0);
    double lon   = get_number (elem, "$.lon", 0.0);
    double dst   = get_number (elem, "$.dst", -1.0);
    double rssi  = get_number (elem, "$.rssi", -1.0);
    double hdg   = get_number (elem, "$.track", 0.0);

    if (flight_str[0] == '\0')
         str_lcpy (flight_str, "-", sizeof(flight_str));
    else str_rtrim (flight_str);

    if (alt == INVALID_ALTITUDE)
       str_lcpy (alt_str, "-", sizeof(alt_str));
    else if (alt == 0.0)
         str_lcpy (alt_str, "ground", sizeof(alt_str));
    else snprintf (alt_str, sizeof(alt_str), "%.0f", Modes.metric ? alt / 3.2828 : alt); /* Feet to meters */

    if (gs >= 0.0)
         snprintf (gs_str, sizeof(gs_str), "%.0f", Modes.metric ? gs * 1.852 : gs); /* Nm/h to km/h */
    else str_lcpy (gs_str, "-", sizeof(gs_str));

    if (dst >= 0.0)
         snprintf (dst_str, sizeof(dst_str), "%.1f", Modes.metric ? dst * 1.852 : dst); /* Nm to km */
    else str_lcpy (dst_str, "-", sizeof(dst_str));

    if (icao_str[0] == '~')  /* a non-ICAO temporary tracking address */
         icao_addr = mg_unhex (icao_str+1) | MODES_NON_ICAO_ADDRESS;
    else icao_addr = mg_unhex (icao_str);

    ac = ac_list_find_or_create (icao_addr);
    if (!ac)
       return (false);

    snprintf (ac->raw_buf, sizeof(ac->raw_buf),
              "%-9s %8s %6s %9.03f %9.03f %8s  %5.0f  %6.2f",
              flight_str, alt_str, gs_str, lat, lon, dst_str, hdg, rssi);

    ac->msg_num++;

    mm_element    *_mm = calloc (sizeof(*_mm), 1);
    modeS_message *mm;

    if (!_mm)
       return (false);

    mm = &_mm->mm;
    str_lcpy (mm->flight, flight_str, sizeof(mm->flight));

    mm->sys_timestamp_msg = MSEC_TIME();
    mm->source = SOURCE_ADSB_FI;

    mm->AC_flags = MODES_ACFLAGS_CALLSIGN_VALID |
                   MODES_ACFLAGS_SQUAWK_VALID   |
                   MODES_ACFLAGS_HEADING_VALID  |
                   MODES_ACFLAGS_LATLON_VALID   |
                   MODES_ACFLAGS_LLEITHER_VALID |
                   MODES_ACFLAGS_CATEGORY_VALID;

    mm->addr          = icao_addr;
    mm->sig_level     = rssi;
    mm->heading       = hdg;
    mm->identity      = atoi (squawk_str);
    mm->category      = mg_unhex (category_str);
    mm->velocity      = gs;
    mm->position.lat  = lat;
    mm->position.lon  = lon;
/*  mm->aircraft_type = ??  */

    if (gs == 0.0)
         mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;
    else mm->AC_flags |= MODES_ACFLAGS_SPEED_VALID;

    mm->altitude = alt;
    if (alt != INVALID_ALTITUDE)
       mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;

    mm->vert_rate = vrate;
    mm->AC_flags |= MODES_ACFLAGS_VERTRATE_VALID;

    LIST_ADD_TAIL (mm_element, &g_data.mm_list, _mm);
  }
  return (g_data.stats.total_last > 0);
}

/**
 * Dump the last json-data to file.
 */
static void json_dump (const char *data)
{
  static uint32_t file_num = 1;
  char   tmp_file [MAX_PATH];

  if (!(Modes.debug & DEBUG_NET2))
     return;

  snprintf (tmp_file, sizeof(tmp_file), "%s\\adsb-fi-%u.json", Modes.tmp_dir, file_num++);
  FILE *f = fopen (tmp_file, "w+t");
  if (f)
  {
    fwrite (data, 1, strlen(data), f);
    fclose (f);
  }
}

#if defined(__DOXYGEN__)
static void CALLBACK wininet_callback (
                       HINTERNET h2,
                       DWORD_PTR context,
                       DWORD     status,
                       void     *status_info,
                       DWORD     status_info_len)
{
  if (status == INTERNET_STATUS_REQUEST_SENT &&
      status_info && status_info_len >= sizeof(DWORD))
  {
    g_data.stats.bytes_sent += *(DWORD*) status_info;
  }
  (void) h2;
  (void) context;
}
#endif

/**
 * Send the request to `g_data.url` and return response data.
 * Or NULL on failure.
 */
static char *send_request (void)
{
  uint64_t now = MSEC_TIME();
  char    *data = download_to_buf (g_data.url);

  if (!data)
  {
    LOG_FILEONLY ("Downloaded no data for %s!\n", g_data.url);
    g_data.stats.wininet_err++;
    return (NULL);
  }

  /**
   * We do not know the full size sent by WinInet.
   * To do this we need to add:
   * ```
   *  (*p_InternetSetStatusCallback) (ctx->h2, wininet_callback);
   * ```
   * and update the below count in our `wininet_callback()` above.
   */
  g_data.stats.bytes_sent += strlen (g_data.url) + 100;
  g_data.stats.requests_sent++;

  if (download_status() != 200)
  {
    LOG_FILEONLY ("HTTP %d\n", download_status());
    g_data.stats.http_err++;
    FREE (data);
  }
  else
  {
    size_t sz = strlen (data);
    LOG_FILEONLY ("HTTP 200: got %zd bytes in %llu msec\n", sz, MSEC_TIME() - now);
    g_data.stats.bytes_recv += sz;
    g_data.stats.responses_recv++;
  }
  return (data);
}
