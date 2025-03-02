/**
 *\file     geo.c
 * \ingroup Misc
 * \brief   Geographic position stuff.
 */
#include "misc.h"
#include "aircraft.h"
#include "geo.h"

/**
 * \def EARTH_RADIUS
 * Earth's radius in meters. Assuming a sphere.
 * Approx. 40.000.000 / TWO_PI meters.
 */
#define EARTH_RADIUS  6371000.0

/**
 * From SDRangel's 'sdrbase/util/azel.cpp':
 *
 * Convert geodetic latitude to geocentric latitude;
 * angle from centre of Earth between the point and equator.
 *
 * \ref https://en.wikipedia.org/wiki/Latitude#Geocentric_latitude
 *
 * \param[in] lat  The geodetic latitude in radians.
 */
static double geocentric_latitude (double lat)
{
  double e2 = 0.00669437999014;

  return atan ((1.0 - e2) * tan(lat));
}

/**
 * Try to figure out some issues with cartesian position going crazy.
 * Ignore the `z` axis (just print level above earth).
 */
static void check_cart (const struct aircraft *a, const cartesian_t *c, double heading, unsigned line)
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

    LOG_FILEONLY ("geo.c(%u): ICAO: %s, x=%.0f, y=%.0f, z=%.0f, heading=%.3f.\n",
                  line, ICAO, x, y, z, M_PI * heading / 180);
//  abort();
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
  geo_lat = geocentric_latitude (lat);

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
 * \ref https://keisan.casio.com/exec/system/1359533867
 *
 * This is good explanation of the various coordinate systems:
 * \ref https://mathworld.wolfram.com/SphericalCoordinates.html
 */
bool geo_cartesian_to_spherical (const struct aircraft *a, const cartesian_t *cart, pos_t *pos)
{
  pos_t  _pos;
  double h = hypot (cart->c_x, cart->c_y);

  if (h < SMALL_VAL)
  {
    LOG_FILEONLY ("geo.c(%u): ICAO: %06X, c_x=%.0f, c_y=%.0f, heading=%.0f.\n",
                  __LINE__, a ? a->addr : 0, cart->c_x, cart->c_y, M_PI * a->heading_rad / 180);
    return (false);
  }

  check_cart (a, cart, a->heading_rad, __LINE__);

  /* We do not need this; close to EARTH_RADIUS.
   *
   * double radius = sqrt (cart->c_x * cart->c_x + cart->c_y * cart->c_y + cart->c_z * cart->c_z);
   */
  _pos.lon = 180.0 * atan2 (cart->c_y, cart->c_x) / M_PI;
  _pos.lat = 180.0 * atan2 (h, cart->c_z) / M_PI;
  *pos = _pos;
  return (VALID_POS(_pos));
}

/**
 * Return the distance between 2 Cartesian points.
 */
double geo_cartesian_distance (const struct aircraft *a, const cartesian_t *c1, const cartesian_t *c2)
{
  static double old_rc = 0.0;
  double delta_X, delta_Y, rc;

  check_cart (a, c1, 0.0, __LINE__);
  check_cart (a, c2, 0.0, __LINE__);

  delta_X = c2->c_x - c1->c_x;
  delta_Y = c2->c_y - c1->c_y;

  rc = hypot (delta_X, delta_Y);   /* sqrt (delta_X*delta_X, delta_Y*delta_Y) */

//assert (fabs(rc - old_rc) < 6000.0);  /* 6 km */
  old_rc = rc;
  (void) old_rc;
  return (rc);
}

/**
 * Return the closest of `val1` and `val2` to `val`.
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
 * \ref https://en.wikipedia.org/wiki/Great-circle_distance
 */
double geo_great_circle_dist (pos_t pos1, pos_t pos2)
{
  double lat1 = (TWO_PI * pos1.lat) / 360.0;  /* convert to radians */
  double lon1 = (TWO_PI * pos1.lon) / 360.0;
  double lat2 = (TWO_PI * pos2.lat) / 360.0;
  double lon2 = (TWO_PI * pos2.lon) / 360.0;
  double dlat = fabs (lat2 - lat1);
  double dlon = fabs (lon2 - lon1);
  double a;

  if (dlat < SMALL_VAL && dlon < SMALL_VAL)
  {
    /*
     * Use haversine for small distances.
     */
    a = sin (dlat/2) * sin (dlat/2) + cos (lat1) * cos (lat2) * sin (dlon/2) * sin (dlon/2);
    return (EARTH_RADIUS * 2 * atan2 (sqrt(a), sqrt(1.0 - a)));
  }

  a = sin (lat1) * sin (lat2) + cos (lat1) * cos (lat2) * cos (fabs(lon1 - lon2));

  /* Radius of the Earth * 'arcosine of angular distance'.
   */
  return (EARTH_RADIUS * acos(a));
}

