/**
 *\file     geo.c
 * \ingroup Misc
 * \brief   Geographic position stuff.
 */
#include "misc.h"
#include "aircraft.h"
#include "geo.h"

/**
 * From SDRangel's 'sdrbase/util/azel.cpp':
 *
 * Convert geodetic latitude to geocentric latitude;
 * angle from centre of Earth between the point and equator.
 *
 * \sa https://en.wikipedia.org/wiki/Latitude#Geocentric_latitude
 *
 * \param[in] lat  The geodetic latitude in radians.
 */
double geo_centric_latitude (double lat)
{
  double e2 = 0.00669437999014;

  return atan ((1.0 - e2) * tan(lat));
}

/**
 * Try to figure out some issues with cartesian position going crazy.
 * Ignore the `z` axis (just print level above earth).
 */
static void check_cart (const aircraft *a, const cartesian_t *c, double heading, unsigned line)
{
  if (fabs(c->c_x) > EARTH_RADIUS || fabs(c->c_y) > EARTH_RADIUS)
  {
    double x = c->c_x / 1E3;
    double y = c->c_y / 1E3;
    double z = (EARTH_RADIUS - c->c_z) / 1E3;
    char   ICAO [10] = { "?" };

    Modes.stat.cart_errors++;
    if (a)
       snprintf (ICAO, sizeof(ICAO), "%06X", a->addr);

    LOG_FILEONLY2 ("geo.c(%u): ICAO: %s, x=%.0f, y=%.0f, z=%.0f, heading=%.3f.\n",
                   line, ICAO, x, y, z, M_PI * heading / 180);
  }
}

/**
 * Convert spherical coordinate to cartesian.
 * Also calculates radius and a normal vector.
 *
 * \param[in]  a     the aircraft in question
 * \param[in]  pos   The position on the Geoid (in degrees).
 * \param[out] cart  The position on Cartesian form (x, y, z).
 */
void geo_spherical_to_cartesian (const struct aircraft *a, const pos_t *pos, cartesian_t *cart)
{
  double lat, lon, geo_lat;
  pos_t _pos = *pos;

  ASSERT_POS (_pos);
  lat  = (M_PI * _pos.lat) / 180.0;
  lon  = (M_PI * _pos.lon) / 180.0;
  geo_lat = geo_centric_latitude (lat);

  cart->c_x = EARTH_RADIUS * cos (lon) * cos (geo_lat);
  cart->c_y = EARTH_RADIUS * sin (lon) * cos (geo_lat);
  cart->c_z = EARTH_RADIUS * sin (geo_lat);
  check_cart (a, cart, 0.0, __LINE__);
}

/**
 * Convert cartesian coordinate to spherical.
 *
 * \param[in]  a     the aircraft in question
 * \param[in]  cart  the aircraft's cartesian position (x, y, z)
 * \param[out] pos   the resulting latitude and longitude position of the aircraft.
 *
 * \retval true   if the input and/or output `_pos` are okay.
 * \retval false  otherwise.
 *
 * This link is dead (but present in `archive.org`):
 * \sa https://keisan.casio.com/exec/system/1359533867
 *
 * This is good explanation of the various coordinate systems:
 * \sa https://mathworld.wolfram.com/SphericalCoordinates.html
 */
bool geo_cartesian_to_spherical (const struct aircraft *a, const cartesian_t *cart, pos_t *pos)
{
  pos_t _pos;
  double radians;

  if (a->heading > 180.0)
       radians = M_PI * (a->heading - 360.0) / 180.0;
  else radians = (M_PI * a->heading) / 180.0;

#if 1
  double h = hypot (cart->c_x, cart->c_y);

  if (h < SMALL_VAL)
  {
    LOG_FILEONLY ("geo.c(%u): ICAO: %06X, c_x=%.0f, c_y=%.0f, heading=%.0f.\n",
                  __LINE__, a ? a->addr : 0, cart->c_x, cart->c_y, M_PI * radians / 180.0);
    return (false);
  }

  check_cart (a, cart, radians, __LINE__);

  /* We do not need this; close to EARTH_RADIUS.
   *
   * double radius = sqrt (cart->c_x * cart->c_x + cart->c_y * cart->c_y + cart->c_z * cart->c_z);
   */
  _pos.lon = 180.0 * atan2 (cart->c_y, cart->c_x) / M_PI;
  _pos.lat = 180.0 * atan2 (h, cart->c_z) / M_PI;

#else
  /*
   * https://www.omnicalculator.com/math/spherical-coordinates
   * https://math.stackexchange.com/questions/2444965/relationship-between-cartesian-velocity-and-polar-velocity
   */
  double radius = sqrt (cart->c_x * cart->c_x + cart->c_y * cart->c_y + cart->c_z * cart->c_z);
  double theta  = atan2 (cart->c_y, cart->c_x);  /* azimuth angle */
  double phi    = acos (cart->c_z / radius);     /* polar angle */

  _pos.lon = (180.0 * theta) / M_PI;
  _pos.lat = (180.0 * phi) / M_PI;

  (void) a;
#endif

  *pos = _pos;
  return (VALID_POS(_pos));
}

/**
 * Return the distance between 2 Cartesian points.
 */
double geo_cartesian_distance (const struct aircraft *a, const cartesian_t *c1, const cartesian_t *c2)
{
  double d_X, d_Y;

  check_cart (a, c1, 0.0, __LINE__);
  check_cart (a, c2, 0.0, __LINE__);

  d_X = c2->c_x - c1->c_x;
  d_Y = c2->c_y - c1->c_y;

  return hypot (d_X, d_Y);   /* sqrt (d_X*d_X, d_Y*d_Y) */
}

/**
 * Return the closest of `val1` and `val2` to `val`.
 *
 * No longer used.
 */
double geo_closest_to (double val, double val1, double val2)
{
  double diff1 = fabs (val1 - val);
  double diff2 = fabs (val2 - val);

  return (diff2 > diff1 ? val1 : val2);
}

/**
 * Distance between 2 points on a spherical earth.
 * This has up to 0.5% error because the earth isn't actually spherical
 * (but we don't use it in situations where that matters)
 *
 * \sa https://en.wikipedia.org/wiki/Great-circle_distance
 */
double geo_great_circle_dist (const pos_t *pos1, const pos_t *pos2)
{
  double lat1 = (M_PI * pos1->lat) / 180.0;  /* convert to radians */
  double lon1 = (M_PI * pos1->lon) / 180.0;
  double lat2 = (M_PI * pos2->lat) / 180.0;
  double lon2 = (M_PI * pos2->lon) / 180.0;
  double dlat = fabs (lat2 - lat1);
  double dlon = fabs (lon2 - lon1);
  double a;

  if (dlat < SMALL_VAL && dlon < SMALL_VAL)
  {
    /* Use haversine for small distances.
     */
    a = sin (dlat/2) * sin (dlat/2) + cos (lat1) * cos (lat2) * sin (dlon/2) * sin (dlon/2);
    return (EARTH_RADIUS * 2 * atan2 (sqrt(a), sqrt(1.0 - a)));
  }

  a = sin (lat1) * sin (lat2) + cos (lat1) * cos (lat2) * cos (fabs(lon1 - lon2));

  /* Radius of the Earth * 'arcosine of angular distance'.
   */
  return (EARTH_RADIUS * acos(a));
}

/**
 * Return the bearing between 2 points on a spherical earth.
 *
 * \retval an angle in range [0 ... 360]
 * The clockwise angle from north.
 */
double geo_get_bearing (const pos_t *pos1, const pos_t *pos2)
{
  double lat0 = pos1->lat * M_PI / 180.0;
  double lon0 = pos1->lon * M_PI / 180.0;
  double lat1 = pos2->lat * M_PI / 180.0;
  double lon1 = pos2->lon * M_PI / 180.0;
  double dlon = (lon1 - lon0);
  double x = (cos(lat0) * sin(lat1)) -
             (sin(lat0) * cos(lat1) * cos(dlon));
  double y = sin (dlon) * cos (lat1);
  double deg = atan2 (y, x) * 180.0 / M_PI;

  if (deg < 0.0)
     deg += 360.0;
  return (deg);
}

/**
 * Return the short name of a bearing.
 *
 * \param in  an angle in range [0 ... 360]
 * \retval    it's name
 *
 * \sa https://www.quora.com/What-direction-is-North-by-Northwest
 */
const char *geo_bearing_name (double bearing)
{
  static const char *names[16] = { "N",  "NNE",
                                   "NE", "ENE",
                                   "E",  "ESE",
                                   "SE", "SSE",
                                   "S",  "SSW",
                                   "SW", "WSW",
                                   "W",  "WNW",
                                   "NW", "NNW"
                                 };
  int idx = (int) floor (bearing / 22.5);

  if (idx < 0 || idx >= DIM(names))
     return ("?");
  return names [idx];
}
