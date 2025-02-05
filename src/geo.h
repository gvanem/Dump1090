/**\file    geo.h
 * \ingroup Misc
 * \brief   Geographic position stuff.
 */
#ifndef _GEO_H
#define _GEO_H

#include <assert.h>

/**
 * Spherical position: <br>
 * Latitude (North-South) and Longitude (East-West) coordinates. <br>
 *
 * A position on a Geoid. (ignoring altitude).
 */
typedef struct pos_t {
        double lat;   /* geodetic latitude */
        double lon;
      } pos_t;

/**
 * A point in Cartesian coordinates.
 */
typedef struct cartesian_t {
        double c_x;
        double c_y;
        double c_z;
      } cartesian_t;

/**
 * \def SMALL_VAL
 * \def BIG_VAL
 * \def VALID_POS()
 * \def ASSERT_POS()
 *
 * Simple check for a valid geo-position
 */
#define SMALL_VAL        0.0001
#define BIG_VAL          9999999.0
#define VALID_POS(pos)   (fabs(pos.lon) >= SMALL_VAL && fabs(pos.lon) < 180.0 && \
                          fabs(pos.lat) >= SMALL_VAL && fabs(pos.lat) < 90.0)

#define ASSERT_POS(pos)  do {                                         \
                           assert (pos.lon >= -180 && pos.lon < 180); \
                           assert (pos.lat >= -90  && pos.lat < 90);  \
                         } while (0)

struct aircraft;

void   geo_spherical_to_cartesian (const struct aircraft *a, const pos_t *pos, cartesian_t *cart);
bool   geo_cartesian_to_spherical (const struct aircraft *a, const cartesian_t *cart, pos_t *pos);
double geo_cartesian_distance (const struct aircraft *a, const cartesian_t *c1, const cartesian_t *c2);
double geo_great_circle_dist (pos_t pos1, pos_t pos2);
double geo_closest_to (double val, double val1, double val2);

#endif /* _GEO_H */
