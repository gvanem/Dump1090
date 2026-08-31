/**\file    adsb-fi.c
 * \ingroup Samplers
 * \brief   Fetches live aircraft data from the adsb.fi opendata API.
 */
#include "misc.h"
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
 * \def RAW_BUF_TIME
 * The time between calls to `print_raw()`.
 */
#define RAW_BUF_TIME 1000

/**
 * \def ADSB_FI_URL_FORMAT
 * The URL format to use. When formatted it looks like:
 *   https://opendata.adsb.fi/api/v3/lat/60.542345/lon/-5.00233/dist/150
 *
 * \ref https://github.com/adsbfi/opendata
 */
#define ADSB_FI_URL_FORMAT  "https://opendata.adsb.fi/api/v3/lat/%.6f/lon/%.6f/dist/%u"

/**
 * \def AC_LIST_INCREMENT
 * `realloc()` increment in `ac_list_find_or_create()`.
 */
#define AC_LIST_INCREMENT 100

/**
 * \typedef ac_element
 * Raw-data we add to `g_data.ac_list`
 */
typedef struct ac_element {
        uint32_t  addr;
        uint32_t  msg_num;
        bool      present;
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
        uint64_t  bytes_sent;      /**< total request bytes sent */
        uint64_t  bytes_recv;      /**< total response bytes received */
        uint32_t  requests_sent;   /**< total requests sent */
        uint32_t  responses_recv;  /**< total responses received */
        uint32_t  http_err;        /**< total HTTP != 200 */
        uint32_t  wininet_err;     /**< total WinInet problems */
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
        bool          print_old;        /**< print old `ac_list::raw_buf` elements? */
        adsb_fi_stats stats;            /**< accumulated statistics */
        char         *url;              /**< the URL we send to */
        char         *data;             /**< the last data we got */
        uint32_t      ac_list_num;      /**< number of ALL `ac_element` in `ac_list` */
        char         *ac_list_end;      /**< points to end of `ac_list` */
        ac_element   *ac_list;          /**< list of `ac_element` */
        mm_element   *mm_list;          /**< list of `mm_element` */
     } adsb_fi_priv;

static adsb_fi_priv g_data;

static char    *send_request (void);
static uint32_t process_response (mg_str body);
static void     print_raw (void);
static void     json_dump (const char *data);
static int      compare_on_addr (const void *_a, const void *_b);

/**
 * Set default values unless set from config-callbacks.
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
  bool rc = download_init_once();

  if (!g_data.max_distance_set)
     g_data.max_distance = MAX_DISTANCE;

  if (!g_data.sleep_time_set)
     g_data.sleep_time = MIN_SLEEP_TIME;

  g_data.print_old = false;

  TRACE ("max_distance: %u miles, sleep_time: %u msec\n",
         g_data.max_distance, g_data.sleep_time);

  return (rc);
}

void adsb_fi_exit (void)
{
  mm_element *mm, *mm_next;

  g_data.ac_list_num = 0;
  FREE (g_data.ac_list);
  FREE (g_data.url);

  for (mm = g_data.mm_list; mm; mm = mm_next)
  {
    mm_next = mm->next;
    LIST_DELETE (mm_element, &g_data.mm_list, mm);
    free (mm);
  }
}

/**
 * Return number of planes received in last `process_response()`.
 */
uint32_t adsb_fi_num_planes (void)
{
  return (g_data.stats.total_last);
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
    LOG_FILEONLY ("Illegal 'adsb-fi-max-distance: %s' miles\n", arg);
  }
  return (true);
}

/**
 * Return max plane distane we query for.
 * In km if `Modes.metric == true`.
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
    LOG_FILEONLY ("Illegal 'adsb-fi-sleep_time: %s'\n", arg);
  }
  return (true);
}

/**
 * Called from `data_thread_fn()` to request JSON-data from `g_data.url`.
 * Process JSON-array response and add ICAO addresses to `g_data.ac_list`.
 */
void adsb_fi_read_loop (void)
{
  while (!Modes.exit)
  {
    /* Flush and print any raw-buffers we've got.
     * Once every `RAW_BUF_TIME`.
     */
    if (!Modes.interactive && !Modes.only_addr)
    {
      static uint64_t last_update;
      uint64_t now = MSEC_TIME();

      /* update-time expired?
       */
      if ((now - last_update) >= RAW_BUF_TIME)
      {
        print_raw();
        last_update = now;
      }
    }

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

    g_data.data = send_request();
    if (g_data.data)
    {
      g_data.stats.total_last = process_response (mg_str(g_data.data));
      if (g_data.stats.total_last > 0)
      {
        qsort (g_data.ac_list, g_data.ac_list_num, sizeof(*g_data.ac_list), compare_on_addr);
        json_dump (g_data.data);
      }
      free (g_data.data);
    }

    /* Call `Sleep(sleep_time)` (minimum 1 sec) between requests.
     * Otherwise we will be blacklisted :-(
     */
    Sleep (g_data.sleep_time);
  }
}

/**
 * For all `g_data.mm_list` messages in queue,
 * call `modeS_user_message()` to create the real planes.
 */
void adsb_fi_poll (void)
{
  mm_element *mm, *mm_next;

  for (mm = g_data.mm_list; mm; mm = mm_next)
  {
    modeS_user_message (&mm->mm);
    mm_next = mm->next;
    LIST_DELETE (mm_element, &g_data.mm_list, mm);
    free (mm);
  }
}

/**
 * Called from `show_statistics()` to print statistics collected here.
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
 * In non-interactive mode and w/o the `--only-addr` option,
 * print the raw plane information collected. Also print planes
 * that has gone absent.
 *
 * Called from `adsb_fi_read_loop()`. I.e. the `data_thread_fn()`.
 */
static void print_raw (void)
{
  int         i, len;
  uint32_t    addr;
  ac_element *ac;

  printf ("Aircrafts in range: %u / %u, max-dist: %u %s\n",
          g_data.stats.total_last, g_data.ac_list_num,
          adsb_fi_get_max_distance(), Modes.metric ? "km" : "Nm");

  len = printf ("%-8s %-9s %8s %6s %9s %9s  %7s",
                "ICAO", "Flight", "Alt", "GS", "Lat", "Lon",
                Modes.metric ? "Km" : "Nm");

  len += printf ("   %4s    %4s   %s  %s\n",
                 "Hdg", "RSSI", "Num", g_data.print_old ? "Present" : "");
  len -= 33;

  printf ("-------------------------------------------"
          "-------------------------------------------%s\n",
          g_data.print_old ? "-------" : "");

  for (i = 0; i < g_data.ac_list_num; i++)
  {
    char buf      [100];
    char addr_buf [10];
    char present  [10] = "";

    ac   = g_data.ac_list + i;
    addr = ac->addr;

    if (!g_data.print_old && !ac->present)
       continue;

    if (addr & MODES_NON_ICAO_ADDRESS)
    {
      addr &= ~MODES_NON_ICAO_ADDRESS;
      addr_buf[0] = '~';
      _itoa (addr, addr_buf+1, 16);
    }
    else
      _itoa (addr, addr_buf, 16);

    if (g_data.print_old)
       _itoa (ac->present, present, 10);
    ac->present = false;

    snprintf (buf, sizeof(buf), "%-7s  %-*s %5u  %s", strupr(addr_buf), len, ac->raw_buf, ac->msg_num, present);
    printf ("%s\n", buf);
  }
  puts ("");
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
 * Returns a numeric field, or 'fallback' if missing or not a number.
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
  size_t sz;

  if (!tok.buf)
     return (fallback);

  if (tok.buf[0] == '"')     /* value is "ground" */
     return (0.0);

  sz = tok.len + 1;
  if (sz > sizeof(val))
     sz = sizeof (val);

  str_lcpy (val, tok.buf, sz);
  return strtod (val, &end);
}

/**
 * The compare functions for `qsort()` and `bsearch()`.
 */
static int compare_on_addr (const void *_a, const void *_b)
{
  const ac_element *a = (const ac_element*) _a;
  const ac_element *b = (const ac_element*) _b;

  if (a->addr < b->addr)
     return (-1);
  if (a->addr > b->addr)
     return (1);
  return (0);
}

/**
 * Find an existing element for `addr` or create a new one.
 */
static ac_element *ac_list_find_or_create (uint32_t addr)
{
  ac_element *ac = NULL;

  if (g_data.ac_list_num > 0)
     ac = bsearch (&addr, g_data.ac_list, g_data.ac_list_num,
                   sizeof(*g_data.ac_list), compare_on_addr);
  if (ac)
     return (ac);

  /* init and/or check for enough space
   */
  if (!g_data.ac_list_end ||
      g_data.ac_list_end - (char*)g_data.ac_list < g_data.ac_list_num * sizeof(*ac))
  {
    size_t new_sz = (g_data.ac_list_num + AC_LIST_INCREMENT) * sizeof(*ac);
    char  *more   = realloc (g_data.ac_list, new_sz);

    if (!more)
       return (NULL);

    g_data.ac_list     = (ac_element*) more;
    g_data.ac_list_end = more + new_sz;
    ac = (ac_element*) more + (g_data.ac_list_num * sizeof(*ac));
  }
  else
    ac = g_data.ac_list + g_data.ac_list_num;

  ac->msg_num = 0; /* 1st raw message for this ICAO address */
  g_data.ac_list_num++;
  return (ac);
}

/**
 * Parse received JSON data and add or modify elements to `g_data.ac_list`.
 */
static uint32_t process_response (mg_str body)
{
  int      ofs1;
  uint32_t num = 0;
  mg_str   elem;
  mg_str   ac_array = mg_json_get_tok (body, "$.ac");

  if (!ac_array.buf || ac_array.buf[0] != '[')
  {
    LOG_FILEONLY ("No 'ac' array found in 'body'?!\n");
    return (num);
  }

  ofs1 = 0;
  while ((ofs1 = mg_json_next(ac_array, ofs1, NULL, &elem)) != 0)
  {
    ac_element *ac;
    uint32_t    icao_addr, ofs2 = 0;
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
    double vrate = get_number (elem, "$.baro_rate", 0.0);       /* correct field? */
    double gs    = get_number (elem, "$.gs", -1.0);             /* ground speed */
    double lat   = get_number (elem, "$.lat", 0.0);             /* latitude */
    double lon   = get_number (elem, "$.lon", 0.0);             /* longitude */
    double dst   = get_number (elem, "$.dst", -1.0);            /* distance to homepos */
    double rssi  = get_number (elem, "$.rssi", -1.0);           /* signal level */
    double hdg   = get_number (elem, "$.true_heading", 360.0);  /* heading or track */

    if (hdg == 360.0)
       hdg = get_number (elem, "$.track", 0.0);   /* not on "ground" */

    if (flight_str[0] != '-')
    {
      str_rtrim (flight_str);
      if (flight_str[0] == '.')  /* Fix e.g. ".N966JT" -> "N966JT" */
         ofs2 = 1;
    }

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
         icao_addr = mg_unhex (icao_str + 1) | MODES_NON_ICAO_ADDRESS;
    else icao_addr = mg_unhex (icao_str);

    ac = ac_list_find_or_create (icao_addr);
    if (!ac)
       return (num);

    ac->msg_num++;
    ac->addr = icao_addr;
    ac->present = true;

    snprintf (ac->raw_buf, sizeof(ac->raw_buf),
              "%-9s %8s %6s %9.03f %9.03f %8s  %5.0f  %6.2f",
              flight_str + ofs2, alt_str, gs_str, lat, lon, dst_str, hdg, rssi);

    modeS_message *mm;
    mm_element    *_mm = calloc (sizeof(*_mm), 1);

    if (!_mm)
       return (num);

    mm = &_mm->mm;

    mm->sys_timestamp_msg = MSEC_TIME();
    mm->source = SOURCE_ADSB_FI;

    mm->AC_flags = MODES_ACFLAGS_LATLON_VALID |
                   MODES_ACFLAGS_HEADING_VALID |
                   MODES_ACFLAGS_LLEITHER_VALID;

    mm->addr          = icao_addr;
    mm->sig_level     = rssi;
    mm->heading       = hdg;
    mm->position.lat  = lat;
    mm->position.lon  = lon;
/*  mm->aircraft_type = ?? */

    if (flight_str[0] != '-')
    {
      str_lcpy (mm->flight, flight_str + ofs2, sizeof(mm->flight));
      mm->AC_flags |= MODES_ACFLAGS_CALLSIGN_VALID;
    }

    if (category_str[0] != '-')
    {
      mm->category = mg_unhex (category_str);
      mm->AC_flags |= MODES_ACFLAGS_CATEGORY_VALID;
    }

    if (squawk_str[0] != '-')
    {
      mm->identity = mg_unhex (squawk_str);
      mm->AC_flags |= MODES_ACFLAGS_SQUAWK_VALID;
    }

    mm->velocity = gs;
    if (gs == 0.0)
         mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;
    else mm->AC_flags |= MODES_ACFLAGS_SPEED_VALID;

    mm->altitude = alt;
    if (alt != INVALID_ALTITUDE)
       mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;

    mm->vert_rate = vrate;
    mm->AC_flags |= MODES_ACFLAGS_VERTRATE_VALID;

    LIST_ADD_TAIL (mm_element, &g_data.mm_list, _mm);
    num++;
  }
  return (num);
}

/**
 * Dump the last json-data to file.
 */
static void json_dump (const char *data)
{
  static uint32_t file_num = 1;
  char   tmp_file [MAX_PATH];
  FILE  *f;

  if (!(Modes.debug & DEBUG_NET2))
     return;

  snprintf (tmp_file, sizeof(tmp_file), "%s\\adsb-fi-%u.json", Modes.tmp_dir, file_num++);
  f = fopen (tmp_file, "w+t");
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
  size_t   sz;

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
    sz = strlen (data);
    TRACE ("HTTP 200: got %zd bytes in %llu msec\n", sz, MSEC_TIME() - now);
    g_data.stats.bytes_recv += sz;
    g_data.stats.responses_recv++;
  }
  return (data);
}
