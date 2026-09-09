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
 * \def MIN_SLEEP_TIME_LOL
 * The minimum Sleep() time between requests to "https://api.adsb.lol".
 * Not sure about what the actual low-limit is.
 */
#define MIN_SLEEP_TIME_LOL 5000

/**
 * \def RAW_BUF_TIME
 * Default and minimum time between calls to `print_raw()`.
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
 * \def ADSB_LOL_URL_FORMAT
 * The alternate URL to use.
 */
#define ADSB_LOL_URL_FORMAT  "https://api.adsb.lol/v2/lat/%.6f/lon/%.6f/dist/%u"

/**
 * \def AC_LIST_INCREMENT
 * `realloc()` increment in `ac_list_find_or_create()`.
 */
#define AC_LIST_INCREMENT  1000

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
        uint32_t  total_last;      /**< last good "ac" element number */
        uint64_t  bytes_sent;      /**< total request bytes sent */
        uint64_t  bytes_recv;      /**< total response bytes received */
        uint32_t  requests_sent;   /**< total requests sent */
        uint32_t  responses_recv;  /**< total responses received */
        uint32_t  http_stat;       /**< last HTTP status */
        uint32_t  http_err;        /**< total HTTP != 200 */
        uint32_t  wininet_err;     /**< total WinInet problems */
      } adsb_fi_stats;

/**
 * \typedef adsb_fi_priv
 *
 * Private data for this module.
 */
typedef struct adsb_fi_priv {
        uint32_t         max_distance;     /**< the max distance (in miles) we will query for */
        uint32_t         sleep_time;       /**< time between requests. In milli-sec */
        uint32_t         raw_buf_time;     /**< time between `print_raw()`. In milli-sec */
        bool             print_old;        /**< print old `ac_list::raw_buf` elements? */
        bool             print_non_icao;   /**< print "~XXXX" ICAO addresses? */
        bool             use_adsb_lol;     /**< use `ADSB_LOL_URL_FORMAT`? */
        adsb_fi_stats    stats;            /**< accumulated statistics */
        char            *url;              /**< the URL we send to */
        char            *data;             /**< the last data we got */
        uint32_t         ac_list_num;      /**< number of ALL `ac_element` in `ac_list` */
        size_t           ac_allocated;     /**< allocated size for `ac_list` */
        ac_element      *ac_list;          /**< list of `ac_element` */
        mm_element      *mm_list;          /**< list of `mm_element` */
        CRITICAL_SECTION crit;             /**< For accessing `g_data.mm_list` from 2 threads */
     } adsb_fi_priv;

static adsb_fi_priv g_data;

static void     set_defaults (void);
static char    *send_request (void);
static uint32_t process_response (mg_str body);
static uint32_t print_raw (void);
static void     json_dump (const char *data);
static int      compare_on_addr (const void *_a, const void *_b);

/**
 * Initialize and set default values unless set from config-callbacks.
 *
 * The use of `download_to_buf()` is not very thread-safe.
 * Hence do not close WinInet after `download_to_buf()`.
 * But initialize and keep it open from now on. Since with a
 * large max-distance, we'll get a large number of `AIRPORT_API_PENDING`
 * records in `API_thread_worker()` in "--interactive" mode.
 */
bool adsb_fi_init (void)
{
  bool rc = download_init_once();

  set_defaults();

  if (g_data.use_adsb_lol)
  {
    g_data.sleep_time   = max (g_data.sleep_time, MIN_SLEEP_TIME_LOL);
    g_data.raw_buf_time = max (g_data.raw_buf_time, MIN_SLEEP_TIME_LOL);
  }

  InitializeCriticalSection (&g_data.crit);

  TRACE ("max_distance: %u miles, sleep_time: %u msec, raw_buf_time: %u msec\n",
         g_data.max_distance, g_data.sleep_time, g_data.raw_buf_time);

  TRACE ("print_old: %d, print_non_icao: %d, use_adsb_lol: %d\n",
         g_data.print_old, g_data.print_non_icao, g_data.use_adsb_lol);

  return (rc);
}

void adsb_fi_exit (void)
{
  mm_element *mm, *mm_next;

  EnterCriticalSection (&g_data.crit);
  g_data.ac_list_num = 0;
  FREE (g_data.ac_list);
  FREE (g_data.url);

  for (mm = g_data.mm_list; mm; mm = mm_next)
  {
    mm_next = mm->next;
    LIST_DELETE (mm_element, &g_data.mm_list, mm);
    free (mm);
  }
  LeaveCriticalSection (&g_data.crit);
  DeleteCriticalSection (&g_data.crit);
}

/**
 * Return number of planes received in last good
 * `process_response()`.
 */
uint32_t adsb_fi_num_planes (void)
{
  return (g_data.stats.total_last);
}

/**
 * Return the start of the 2 possible URLs.
 * The scheme + host part only.
 */
const char *adsb_fi_url (bool use_lol)
{
  static char buf [2][100];
  static int  idx = 0;
  const char *url   = use_lol ? ADSB_LOL_URL_FORMAT : ADSB_FI_URL_FORMAT;
  const char *slash = strchr (url + strlen("https://"), '/');
  char       *ret   = buf [idx];

  strncpy (ret, url, slash - url);
  idx = (idx + 1) & 1;   /* use 2 buffers in round-robin */
  return (ret);
}

/**
 * Set all defaults once
 */
static void set_defaults (void)
{
  static bool done = false;

  if (done)
     return;

  done = true;
  g_data.max_distance   = MAX_DISTANCE;
  g_data.sleep_time     = MIN_SLEEP_TIME;
  g_data.raw_buf_time   = RAW_BUF_TIME;
  g_data.print_old      = false;
  g_data.print_non_icao = false;
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

void adsb_fi_use_lol (void)
{
  g_data.use_adsb_lol = true;
}

/**
 * Config-callback for "adsb-fi-max-distance = miles".
 */
bool adsb_fi_max_distance (const char *arg)
{
  char    *end;
  uint64_t val = strtoull (arg, &end, 10);

  set_defaults();

  if (end > arg && val >= MIN_DISTANCE && val <= MAX_DISTANCE)
     g_data.max_distance = (uint32_t) val;
  else
  {
    cfg_illegal_val ("adsb-fi-max-distance", arg);
    LOG_STDERR ("Illegal 'adsb-fi-max-distance: %s' miles\n", arg);
  }
  return (true);
}

/**
 * Config-callback for "adsb-fi-sleep-time = msec".
 */
bool adsb_fi_sleep_time (const char *arg)
{
  char    *end;
  uint64_t val = strtoull (arg, &end, 10);

  set_defaults();

  if (end > arg && val >= MIN_SLEEP_TIME)
     g_data.sleep_time = (uint32_t) val;
  else
  {
    cfg_illegal_val ("adsb-fi-sleep-time", arg);
    LOG_STDERR ("Illegal 'adsb-fi-sleep_time: %s'\n", arg);
  }
  return (true);
}

/**
 * Config-callback for "adsb-fi-raw-buf-time = msec".
 */
bool adsb_fi_raw_buf_time (const char *arg)
{
  char    *end;
  uint64_t val = strtoull (arg, &end, 10);

  set_defaults();

  if (end > arg && val >= RAW_BUF_TIME)
     g_data.raw_buf_time = (uint32_t) val;
  else
  {
    cfg_illegal_val ("adsb-fi-raw-buf-time", arg);
    LOG_STDERR ("Illegal 'adsb-fi-raw-buf-time: %s'\n", arg);
  }
  return (true);
}

/**
 * Config-callback for "adsb-fi-print-old = 0|1".
 */
bool adsb_fi_print_old (const char *arg)
{
  set_defaults();
  g_data.print_old = cfg_true (arg);
  return (true);
}

/**
 * Config-callback for "adsb-fi-print-non-icao = 0|1".
 */
bool adsb_fi_print_non_icao (const char *arg)
{
  set_defaults();
  g_data.print_non_icao = cfg_true (arg);
  return (true);
}

/**
 * Called from `data_thread_fn()` to request JSON-data from `g_data.url`.
 *
 * Process JSON-array response and add ICAO addresses to `g_data.ac_list`
 * and `modeS_message` elements to `g_data.mm_list`.
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
      uint32_t num = 0;

      /* update-time expired?
       */
      if ((now - last_update) >= g_data.raw_buf_time)
      {
        num = print_raw();
        if (num / g_data.raw_buf_time >= 10)
        {
          g_data.raw_buf_time += 100;
          TRACE ("Extending raw_buf_time: %u\n", g_data.raw_buf_time);
        }
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
      g_data.url = mg_mprintf (g_data.use_adsb_lol ? ADSB_LOL_URL_FORMAT : ADSB_FI_URL_FORMAT,
                               Modes.home_pos.lat, Modes.home_pos.lon, g_data.max_distance);
      TRACE ("Fetching from '%s'\n", g_data.url);
    }

    g_data.data = send_request();
    if (g_data.data)
    {
      uint32_t num = process_response (mg_str(g_data.data));

      if (num > 0)
      {
        g_data.stats.total_last = num;
        qsort (g_data.ac_list, g_data.ac_list_num, sizeof(*g_data.ac_list), compare_on_addr);
        json_dump (g_data.data);
      }
      free (g_data.data);
    }

    /* Call `Sleep(sleep_time)` (minimum 1 sec) between requests.
     * Otherwise we'll get a "HTTP 429 Too Many Requests" or similar.
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

  EnterCriticalSection (&g_data.crit);
  for (mm = g_data.mm_list; mm; mm = mm_next)
  {
    modeS_user_message (&mm->mm);
    mm_next = mm->next;
    LIST_DELETE (mm_element, &g_data.mm_list, mm);
    free (mm);
  }
  LeaveCriticalSection (&g_data.crit);
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
 * print the raw plane information collected.
 *
 * Also if `g_data.print_old == true`, print planes that are no
 * longer present.
 *
 * Called from `adsb_fi_read_loop()`. I.e. the `data_thread_fn()`.
 */
static uint32_t print_raw (void)
{
  uint32_t i, num;
  int      len;

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

  for (i = num = 0; i < g_data.ac_list_num; i++)
  {
    char        buf      [100];
    char        addr_buf [10];
    char        present  [10] = "";
    uint32_t    addr;
    ac_element *ac = g_data.ac_list + i;

    if (!g_data.print_old && !ac->present)
       continue;

    addr = ac->addr;
    if (addr & MODES_NON_ICAO_ADDRESS)
    {
      if (!g_data.print_non_icao)
         continue;

      addr_buf[0] = '~';
      _itoa (addr & MODES_ICAO_ADDRESS_MASK, addr_buf+1, 16);
    }
    else
      _itoa (addr, addr_buf, 16);

    if (g_data.print_old)
       _itoa (ac->present, present, 10);

    ac->present = false;
    snprintf (buf, sizeof(buf), "%-7s  %-*s %5u  %s",
              strupr(addr_buf), len, ac->raw_buf, ac->msg_num, present);
    printf ("%s\n", buf);
    num++;
  }
  puts ("");
  return (num);
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
    free (str);
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
  ac_element *ac_more;

  if (g_data.ac_list_num > 0)
     ac = bsearch (&addr, g_data.ac_list, g_data.ac_list_num,
                   sizeof(*g_data.ac_list), compare_on_addr);
  if (ac)
     return (ac);

  /* init and/or check for enough space
   */
  if (g_data.ac_list_num >= g_data.ac_allocated / sizeof(*ac))
  {
    g_data.ac_allocated += sizeof(*ac) * AC_LIST_INCREMENT;
    ac_more = realloc (g_data.ac_list, g_data.ac_allocated);
    TRACE ("Growing to %zd bytes, ac_more: 0x%p\n",
           g_data.ac_allocated, ac_more);

    if (!ac_more)
       return (NULL);

    g_data.ac_list = ac_more;
  }
  ac = g_data.ac_list + g_data.ac_list_num++;
  ac->msg_num = 0;   /* 1st raw message for this ICAO address */
  return (ac);
}

/**
 * Parse received JSON data and modify / add elements to `g_data.ac_list`.
 */
static uint32_t process_response (mg_str body)
{
  int      ofs1 = 0;
  uint32_t num = 0;
  mg_str   elem;
  mg_str   ac_array = mg_json_get_tok (body, "$.ac");

  if (!ac_array.buf || ac_array.buf[0] != '[')
  {
    TRACE ("No 'ac' array found in 'body'?!\n");
    return (0);
  }

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
    double speed = get_number (elem, "$.gs", 0.0);              /* ground speed */
    double lat   = get_number (elem, "$.lat", 0.0);             /* latitude */
    double lon   = get_number (elem, "$.lon", 0.0);             /* longitude */
    double dst   = get_number (elem, "$.dst", 0.0);             /* distance to homepos */
    double rssi  = get_number (elem, "$.rssi", 0.0);            /* signal level */
    double hdg   = get_number (elem, "$.true_heading", 360.0);  /* heading or track */

    if (hdg == 360.0)
       hdg = get_number (elem, "$.track", 0.0);   /* not on "ground" */

    if (flight_str[0] != '-')
    {
      str_rtrim (flight_str);
      if (flight_str[0] == '.')    /* Fix e.g. ".N966JT" -> "N966JT" */
         ofs2 = 1;
    }

    if (alt == INVALID_ALTITUDE)
       str_lcpy (alt_str, "-", sizeof(alt_str));
    else if (alt == 0.0)
         str_lcpy (alt_str, "ground", sizeof(alt_str));
    else snprintf (alt_str, sizeof(alt_str), "%.0f", Modes.metric ? alt / 3.2828 : alt); /* Feet to meters */

    if (speed >= 0.0)
         snprintf (gs_str, sizeof(gs_str), "%.0f", Modes.metric ? speed * 1.852 : speed); /* Nm/h to km/h */
    else str_lcpy (gs_str, "-", sizeof(gs_str));

    if (dst >= 0.0)
         snprintf (dst_str, sizeof(dst_str), "%.1f", Modes.metric ? dst * 1.852 : dst); /* Nm to km */
    else str_lcpy (dst_str, "-", sizeof(dst_str));

    if (icao_str[0] == '~')  /* a non-ICAO temporary tracking address */
         icao_addr = mg_unhex (icao_str + 1) | MODES_NON_ICAO_ADDRESS;
    else icao_addr = mg_unhex (icao_str);

    if ((icao_addr & MODES_NON_ICAO_ADDRESS) && !g_data.print_non_icao)
       continue;

    ac = ac_list_find_or_create (icao_addr);
    if (!ac)
       break;

    ac->msg_num++;
    ac->addr = icao_addr;
    ac->present = true;

    snprintf (ac->raw_buf, sizeof(ac->raw_buf),
              "%-9s %8s %6s %9.03f %9.03f %8s  %5.0f  %6.2f",
              flight_str + ofs2, alt_str, gs_str, lat, lon, dst_str, hdg, rssi);

    modeS_message *mm;
    mm_element    *_mm = calloc (sizeof(*_mm), 1);

    if (!_mm)
       break;

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

    mm->velocity = speed;
    if (speed == 0.0)
         mm->AC_flags |= MODES_ACFLAGS_AOG_VALID;
    else mm->AC_flags |= MODES_ACFLAGS_SPEED_VALID;

    mm->altitude = alt;
    if (alt != INVALID_ALTITUDE)
       mm->AC_flags |= MODES_ACFLAGS_ALTITUDE_VALID;

    mm->vert_rate = vrate;
    mm->AC_flags |= MODES_ACFLAGS_VERTRATE_VALID;

    EnterCriticalSection (&g_data.crit);

    LIST_ADD_TAIL (mm_element, &g_data.mm_list, _mm);
    num++;

    LeaveCriticalSection (&g_data.crit);
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
  double  start_t = get_usec_now();
  size_t  sz;
  char   *data = download_to_buf (g_data.url);

  if (!data)
  {
    TRACE ("Downloaded no data for %s!\n", g_data.url);
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
    TRACE ("HTTP %d\n", download_status());
    g_data.stats.http_stat = download_status();
    g_data.stats.http_err++;
    FREE (data);
  }
  else
  {
    sz = strlen (data);
    TRACE ("HTTP 200: got %zd bytes (%.0f kB/s)\n", sz, 1E3 * sz / (get_usec_now() - start_t));
    g_data.stats.http_stat = 200;
    g_data.stats.bytes_recv += sz;
    g_data.stats.responses_recv++;
  }
  return (data);
}
