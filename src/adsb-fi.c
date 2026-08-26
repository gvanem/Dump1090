/*
 * adsb-fi.c
 *
 * Fetches live aircraft data from the adsb.fi opendata API and parses
 * the JSON response, printing a summary table of aircraft found.
 */
#include "misc.h"
#include "adsb-fi.h"

/**
 * \def MAX_DISTANCE
 * The max distance (in miles) we can query for.
 * max value is 250 miles.
 *
 * \todo make this a .cfg-setting.
 */
#define MAX_DISTANCE 150

/**
 * \def ADSB_FI_URL
 * The URL to send to. Like:
 *   https://opendata.adsb.fi/api/v3/lat/60.542345/lon/-5.00233/dist/150
 *
 * \ref https://github.com/adsbfi/opendata
 */
#define ADSB_FI_URL  "https://opendata.adsb.fi/api/v3/lat/%.6f/lon/%.6f/dist/%d"

/**
 * Performs an HTTP GET on 'url' and returns the body in a malloc'd buffer.
 * Caller must free the returned pointer. Returns NULL on failure.
 */
static char *http_get (const char *url)
{
  char *data;
  int   http_code;

  TRACE ("Fetching: %s\n", url);
  data = download_to_buf (url);
  if (!data)
  {
    TRACE ("Downloaded no data for %s!\n", url);
    return (NULL);
  }

  http_code = download_status();
  if (http_code != 200)
  {
    TRACE ("HTTP request returned status %d\n", http_code);
    free (data);
    return (NULL);
  }
  return (data);
}

/**
 * mg_json helpers
 *
 * mg_json_* functions take a `mg_str` span of raw JSON text and a
 * JSONPath-like query string ("$.field"). Here 'ac' is the raw text
 * span of a single aircraft object pulled out of the top-level "ac"
 * array, so every query below is relative to that object ("$.xxx").
 *
 * Copies a string field into 'out'; falls back to 'fallback' if the
 * field is missing or isn't a string. `mg_json_get_str()` heap-allocates
 * its result, so we free it after copying.
 */
static void get_str (mg_str ac, const char *path, char *out, size_t outsz, const char *fallback)
{
  char *s = mg_json_get_str (ac, path);

  if (s)
  {
    snprintf (out, outsz, "%s", s);
    free (s);
  }
  else
    snprintf (out, outsz, "%s", fallback);
}

/**
 * Returns a numeric field, or 'fallback' if missing / not a number.
 */
static double get_num (struct mg_str ac, const char *path, double fallback)
{
  double val;

  if (mg_json_get_num(ac, path, &val))
     return (val);
  return (fallback);
}

/**
 * alt_baro is usually a number, but the API reports "ground" (a string)
 * for aircraft that are on the ground, so both shapes must be handled.
 */
static void get_alt_str (mg_str ac, char *out, size_t out_sz)
{
  mg_str tok = mg_json_get_tok (ac, "$.alt_baro");
  double val;

  if (!tok.buf)
     snprintf (out, out_sz, "-");
  else if (tok.buf[0] == '"')
     get_str (ac, "$.alt_baro", out, out_sz, "-");
  else
  {
    if (mg_json_get_num(ac, "$.alt_baro", &val))
    {
     if (Modes.metric)
        val /= 3.2828;
      snprintf (out, out_sz, "%d", (int)val);
    }
    else snprintf (out, out_sz, "-");
  }
}

/**
 * The `mg_json*()` functions operate directly on the raw text buffer.
 */
static bool print_aircraft_table (char *_body, size_t _body_len)
{
  int    off = 0;
  long   total;
  mg_str elem;
  mg_str body = { _body, _body_len };
  mg_str ac   = mg_json_get_tok (body, "$.ac");

  if (!ac.buf || ac.buf[0] != '[')
  {
    TRACE ("No 'ac' array found in 'body' data\n");
    return (false);
  }

  total = mg_json_get_long (body, "$.total", 0);

  printf ("Aircraft in range: %ld\n"
          "%-9s %-9s %8s %6s %9s %9s  %5s  %s\n"
          "-----------------------------------------------------------------------------------------------\n",
          total, "HEX", "FLIGHT", "ALT", "GS", "LAT", "LON",
          Modes.metric ? "Km" : "Nm", "RSSI");

  while ((off = mg_json_next(ac, off, NULL, &elem)) != 0)
  {
    char hex [16];
    get_str (elem, "$.hex", hex, sizeof(hex), "?");
    strupr (hex);

    char flight [16];
    get_str (elem, "$.flight", flight, sizeof(flight), "-");
    if (flight[0] == '\0')
         snprintf (flight, sizeof(flight), "-");
    else str_rtrim (flight);

    char alt [16];
    get_alt_str (elem, alt, sizeof(alt));

    double gs   = get_num (elem, "$.gs", -1);
    double lat  = get_num (elem, "$.lat", 0);
    double lon  = get_num (elem, "$.lon", 0);
    double dst  = get_num (elem, "$.dst", -1);
    double rssi = get_num (elem, "$.rssi", -1);
    char   gs_str [16];
    char   dst_str [16];

    if (Modes.metric)
    {
      dst /= 1.60934;
      gs *=  1.60934;
    }

    if (gs >= 0.0)
         snprintf (gs_str,  sizeof(gs_str),  "%.0f", gs);
    else snprintf (gs_str,  sizeof(gs_str),  "-");

    if (dst >= 0.0)
         snprintf (dst_str, sizeof(dst_str), "%.1f", dst);
    else snprintf (dst_str, sizeof(dst_str), "-");

    printf ("%-9s %-9s %8s %6s %9.03f %9.03f %8s  %.2f\n",
            hex, flight, alt, gs_str, lat, lon, dst_str, rssi);
  }
  puts ("");
  return (total > 0);
}

bool adsb_fi_init (void)
{
  return (true);
}

static bool adsb_fi_get (const pos_t *pos)
{
  char  url [256];
  char *body;
  int   rc;

  snprintf (url, sizeof(url), ADSB_FI_URL, pos->lat, pos->lon, MAX_DISTANCE);
  body = http_get (url);
  if (!body)
     return (false);

  rc = print_aircraft_table (body, strlen(body));
  free (body);
  return (rc);
}

bool adsb_fi_read_loop (void)
{
  int rc;

  if (!Modes.home_pos_ok)
  {
    TRACE ("%s(): Modes.home_pos not OK: %.4f / %.4f\n",
           __FUNCTION__, Modes.home_pos.lat, Modes.home_pos.lon);
    return (false);
  }
  rc = adsb_fi_get (&Modes.home_pos);

#if 0
  modeS_message mm;
  modeS_user_message (&mm);
#endif

  return (rc);
}
