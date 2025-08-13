/*
 * Part of dump1090, a Mode S message decoder for RTLSDR devices.
 *
 * Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
 */

/**\file    cpr.c
 * \ingroup Misc
 * \brief   Decoding of **CPR** (*Compact Position Reporting*)
 *          from a `modeS_message`.
 */
#include "aircraft.h"
#include "misc.h"
#include "geo.h"
#include "cpr.h"

static int      CPR_NL_func (double lat);
static unsigned CPR_error;
static unsigned CPR_errline;

static int CPR_decode_airborne (int even_cprlat, int even_cprlon,
                                int odd_cprlat,  int odd_cprlon, bool is_odd, pos_t *out);

static int CPR_decode_surface  (double ref_lat,     double ref_lon,
                                int    even_cprlat, int    even_cprlon,
                                int    odd_cprlat,  int    odd_cprlon, bool is_odd, pos_t *out);

static int CPR_decode_relative (double ref_lat, double ref_lon,
                                int    cprlat,  int    cprlon,
                                bool   is_odd,  bool   surface, pos_t *out);

static bool CPR_speed_check (aircraft            *a,
                             const modeS_message *mm,
                             const pos_t         *pos,
                             uint64_t             now,
                             bool                 surface);

#define CPR_SET_ERR(e)  do {                       \
                          CPR_error = e;           \
                          CPR_errline = __LINE__;  \
                          Modes.stat.cpr_errors++; \
                        } while (0)

static const char *CPR_strerror (void)
{
  const char *err = (CPR_error == EINVAL ? "EINVAL" :
                     CPR_error == ERANGE ? "ERANGE" :
                     CPR_error == E2BIG  ? "E2BIG"  : "?");
  static char buf [50];

  snprintf (buf, sizeof(buf), "CPR_error: %s at line %u", err, CPR_errline);
  return (buf);
}

static int CPR_set_error (int result, aircraft *a, uint64_t now)
{
  if (result >= 0)
     a->seen_pos_EST = now;
  else if (Modes.cpr_trace && CPR_errline)
     LOG_FILEONLY2 ("%s %06X, %s.\n",
                    a->is_helicopter ? "helicopter" : "plane",
                    a->addr, CPR_strerror());
  return (result);
}

/**
 * Helper function for decoding the **CPR**. <br>
 * Always positive MOD operation.
 */
static int CPR_mod_func (int a, int b)
{
  int res = a % b;

  if (res < 0)
     res += b;
  return (res);
}

static double CPR_mod_double (double a, double b)
{
  double res = fmod (a, b);

  if (res < 0)
     res += b;
  return (res);
}

static int CPR_N_func (double lat, bool is_odd)
{
  int nl = CPR_NL_func (lat) - (int)is_odd;

  if (nl < 1)
     nl = 1;
  return (nl);
}

static double CPR_Dlong_func (double lat, bool is_odd)
{
  return (360.0 / CPR_N_func (lat, is_odd));
}

static double CPR_Dlong_func2 (double lat, int is_odd, bool surface)
{
  return (surface ? 90.0 : 360.0) / CPR_N_func (lat, is_odd);
}

/**
 * Helper function for decoding the **CPR** (*Compact Position Reporting*).
 *
 * Calculates **NL** *(lat)*; *Number of Longitude* zone. <br>
 * Given the latitude, this function returns the number of longitude zones between 1 and 59.
 *
 * The NL function uses the precomputed table from 1090-WP-9-14; Table A-21. <br>
 * Refer [The-1090MHz-riddle](./The-1090MHz-riddle.pdf), page 45 for the exact equation.
 */
static int CPR_NL_func (double lat)
{
  if (lat < 0)
     lat = -lat;   /* Table is symmetric about the equator. */

  if (lat > 60.0)
     goto L60;

  if (lat > 44.2)
     goto L42;

  if (lat > 30.0)
     goto L30;

  if (lat < 10.47047130) return (59);
  if (lat < 14.82817437) return (58);
  if (lat < 18.18626357) return (57);
  if (lat < 21.02939493) return (56);
  if (lat < 23.54504487) return (55);
  if (lat < 25.82924707) return (54);
  if (lat < 27.93898710) return (53);
  if (lat < 29.91135686) return (52);

L30:
  if (lat < 31.77209708) return (51);
  if (lat < 33.53993436) return (50);
  if (lat < 35.22899598) return (49);
  if (lat < 36.85025108) return (48);
  if (lat < 38.41241892) return (47);
  if (lat < 39.92256684) return (46);
  if (lat < 41.38651832) return (45);
  if (lat < 42.80914012) return (44);
  if (lat < 44.19454951) return (43);

L42:
  if (lat < 45.54626723) return (42);
  if (lat < 46.86733252) return (41);
  if (lat < 48.16039128) return (40);
  if (lat < 49.42776439) return (39);
  if (lat < 50.67150166) return (38);
  if (lat < 51.89342469) return (37);
  if (lat < 53.09516153) return (36);
  if (lat < 54.27817472) return (35);
  if (lat < 55.44378444) return (34);
  if (lat < 56.59318756) return (33);
  if (lat < 57.72747354) return (32);
  if (lat < 58.84763776) return (31);
  if (lat < 59.95459277) return (30);

L60:
  if (lat < 61.04917774) return (29);
  if (lat < 62.13216659) return (28);
  if (lat < 63.20427479) return (27);
  if (lat < 64.26616523) return (26);
  if (lat < 65.31845310) return (25);
  if (lat < 66.36171008) return (24);
  if (lat < 67.39646774) return (23);
  if (lat < 68.42322022) return (22);
  if (lat < 69.44242631) return (21);
  if (lat < 70.45451075) return (20);
  if (lat < 71.45986473) return (19);
  if (lat < 72.45884545) return (18);
  if (lat < 73.45177442) return (17);
  if (lat < 74.43893416) return (16);
  if (lat < 75.42056257) return (15);
  if (lat < 76.39684391) return (14);
  if (lat < 77.36789461) return (13);
  if (lat < 78.33374083) return (12);
  if (lat < 79.29428225) return (11);
  if (lat < 80.24923213) return (10);
  if (lat < 81.19801349) return (9);
  if (lat < 82.13956981) return (8);
  if (lat < 83.07199445) return (7);
  if (lat < 83.99173563) return (6);
  if (lat < 84.89166191) return (5);
  if (lat < 85.75541621) return (4);
  if (lat < 86.53536998) return (3);
  if (lat < 87.00000000) return (2);
  return (1);
}

int cpr_do_global (struct aircraft *a, const struct modeS_message *mm, uint64_t now, pos_t *new_pos, unsigned *nuc)
{
  int   result;
  int   odd_packet = (mm->AC_flags & MODES_ACFLAGS_LLODD_VALID) != 0;
  bool  surface    = (mm->AC_flags & MODES_ACFLAGS_AOG) != 0;
  pos_t ref_pos    = { 0.0, 0.0 };

  *nuc = (a->even_CPR_nuc < a->odd_CPR_nuc ? a->even_CPR_nuc : a->odd_CPR_nuc); /* worst of the two positions */

  if (surface)
  {
    /* Surface global CPR:
     * find reference location
      */
    if (a->AC_flags & MODES_ACFLAGS_LATLON_REL_OK)  /* Ok to try aircraft relative first */
    {
      ref_pos = a->position;
      if (a->pos_nuc < *nuc)
         *nuc = a->pos_nuc;
    }
    else if (Modes.home_pos_ok)
    {
      ref_pos = Modes.home_pos;
    }
    else
    {
      /* No local reference, give up
       */
      return (-1);
    }
    result = CPR_decode_surface (ref_pos.lat, ref_pos.lon,
                                 a->even_CPR_lat, a->even_CPR_lon,
                                 a->odd_CPR_lat, a->odd_CPR_lon, odd_packet,
                                 new_pos);
  }
  else
  {
    /* airborne global CPR
     */
    result = CPR_decode_airborne (a->even_CPR_lat, a->even_CPR_lon,
                                  a->odd_CPR_lat, a->odd_CPR_lon, odd_packet, new_pos);
  }

  if (result < 0)
  {
    if (mm->AC_flags & MODES_ACFLAGS_FROM_MLAT)
       CPR_TRACE ("%06X: decode failure from MLAT (%d). even: %d %d, odd: %d %d, odd_packet: %s\n",
                  a->addr, result, a->even_CPR_lat, a->even_CPR_lon,
                  a->odd_CPR_lat, a->odd_CPR_lon, odd_packet ? "odd" : "even");
    return CPR_set_error (result, a, now);
  }

  CPR_set_error (0, a, now);

  /* Check max distance
   */
  if (Modes.max_dist > 0 && Modes.home_pos_ok)
  {
    double distance = geo_great_circle_dist (&Modes.home_pos, new_pos);

    if (distance > Modes.max_dist)
    {
      CPR_TRACE ("%06X: global distance check failed (%.3f,%.3f), max dist %.1fkm, actual %.1fkm\n",
                 a->addr, new_pos->lat, new_pos->lon, (double)Modes.max_dist / 1000.0, distance / 1000.0);

      Modes.stat.cpr_global_dist_checks++;
      a->global_dist_checks++;
      return (-2);    /* we consider an out-of-distance value to be bad data */
    }

    a->distance     = distance;
    a->distance_ok  = true;
    a->position_EST = *new_pos;
    a->global_dist_ok++;
    LOG_DISTANCE (a);
  }

  /* For MLAT results, skip the speed check
   */
  if (mm->AC_flags & MODES_ACFLAGS_FROM_MLAT)
     return (result);

  /* Check speed limit
   */
  if (a->pos_nuc >= *nuc && !CPR_speed_check(a, mm, new_pos, now, surface))
  {
    Modes.stat.cpr_global_speed_checks++;
    return (-2);
  }
  return CPR_set_error (result, a, now);
}

int cpr_do_local (struct aircraft *a, const struct modeS_message *mm, uint64_t now, pos_t *new_pos, unsigned *nuc)
{
  /* relative CPR: find reference location
   */
  pos_t  ref_pos;
  double distance_limit = 0.0;
  int    result;
  bool   odd_packet = (mm->AC_flags & MODES_ACFLAGS_LLODD_VALID) != 0;
  bool   surface    = (mm->AC_flags & MODES_ACFLAGS_AOG) != 0;

  *nuc = mm->nuc_p;

  if (a->AC_flags & MODES_ACFLAGS_LATLON_REL_OK)
  {
    ref_pos = a->position;

    if (a->pos_nuc < *nuc)
       *nuc = a->pos_nuc;
  }
  else if (!surface && Modes.home_pos_ok)
  {
    ref_pos = Modes.home_pos;

    /* The cell size is at least 360NM, giving a nominal
     * max distance of 180NM == 333360 m (half a cell).
     */
#define CELL_SIZE 333360

    /* If the receiver distance is more than half a cell, then we must limit
     * this distance further to avoid ambiguity. (e.g. if we receive a position
     * report at 200NM distance, this may resolve to a position at (200-360) = 160NM
     * in the wrong direction).
     */
    if (Modes.max_dist <= CELL_SIZE)
    {
      distance_limit = Modes.max_dist;
    }
    else if (Modes.max_dist < 2*CELL_SIZE)
    {
      distance_limit = 2 * CELL_SIZE - Modes.max_dist;
    }
    else
    {
      return (-1); /* Can't do receiver-centered checks at all */
    }
  }
  else
  {
    /* No local reference, give up */
    return (-1);
  }

  result = CPR_decode_relative (ref_pos.lat, ref_pos.lon,
                                mm->raw_latitude, mm->raw_longitude,
                                odd_packet, surface, new_pos);
  if (result < 0)
     return CPR_set_error (result, a, now);   /* Failure */

  /* Check distance limit if user-specified position is OK
   */
  if (distance_limit > 0 && Modes.home_pos_ok)
  {
    double distance = geo_great_circle_dist (&ref_pos, new_pos);

    if (distance > distance_limit)
    {
      Modes.stat.cpr_local_dist_checks++;
      return (-1);
    }

    a->distance     = distance;
    a->distance_ok  = true;
    a->position_EST = *new_pos;
    LOG_DISTANCE (a);
  }

  /* Check speed limit
   */
  if (a->pos_nuc >= *nuc && !CPR_speed_check(a, mm, new_pos, now, surface))
  {
    Modes.stat.cpr_local_speed_checks++;
    return (-1);
  }
  return CPR_set_error (0, a, now);   /* Okay */
}

/*
 * This algorithm comes from: <br>
 * http://www.lll.lu/~edward/edward/adsb/DecodingADSBposition.html.
 *
 * A few remarks:
 *
 * \li 131072 is 2^17 since CPR latitude and longitude are encoded in 17 bits.
 * \li We assume that we always received the odd packet as last packet for
 *     simplicity. This may provide a position that is less fresh of a few seconds.
 */
static int CPR_decode_airborne (int even_cprlat, int even_cprlon, int odd_cprlat, int odd_cprlon,
                                bool is_odd, pos_t *out)
{
  double air_dlat0 = 360.0 / 60.0;
  double air_dlat1 = 360.0 / 59.0;
  double lat0      = even_cprlat;
  double lat1      = odd_cprlat;
  double lon0      = even_cprlon;
  double lon1      = odd_cprlon;
  double rlat, rlon;

  /* Compute the Latitude Index "j"
   */
  int    j     = (int) floor (((59*lat0 - 60*lat1) / 131072) + 0.5);
  double rlat0 = air_dlat0 * (CPR_mod_func(j, 60) + lat0 / 131072);
  double rlat1 = air_dlat1 * (CPR_mod_func(j, 59) + lat1 / 131072);

  out->lat = out->lon = 0.0;

  CPR_error = 0;

  if (rlat0 >= 270)
     rlat0 -= 360;

  if (rlat1 >= 270)
     rlat1 -= 360;

  /* Check to see that the latitude is in range: -90 .. +90
   */
  if (rlat0 < -90 || rlat0 > 90 || rlat1 < -90 || rlat1 > 90)
  {
    CPR_SET_ERR (EINVAL);
    return (-2);       /* bad data */
  }

  /* Check that both are in the same latitude zone, or abort.
   */
  if (CPR_NL_func(rlat0) != CPR_NL_func(rlat1))
  {
    CPR_SET_ERR (ERANGE);
    CPR_errline = 0;   /* ignore this since it's too frequent */
    return (-1);       /* positions crossed a latitude zone, try again later */
  }

  /* Compute ni and the Longitude Index "m"
   */
  if (is_odd)
  {
    int ni = CPR_N_func (rlat1, true);
    int m = (int) floor ((((lon0 * (CPR_NL_func(rlat1) - 1)) -
                          (lon1 * CPR_NL_func(rlat1))) / 131072.0) + 0.5);

    rlon = CPR_Dlong_func2 (rlat1, true, false) * (CPR_mod_func(m, ni)+lon1/131072);
    rlat = rlat1;
  }
  else
  {
    int ni = CPR_N_func (rlat0, false);
    int m = (int) floor ((((lon0 * (CPR_NL_func(rlat0)-1)) -
                         (lon1 * CPR_NL_func(rlat0))) / 131072) + 0.5);
    rlon = CPR_Dlong_func2 (rlat0, false, false) * (CPR_mod_func(m, ni)+lon0/131072);
    rlat = rlat0;
  }

  /* Renormalize to -180 .. +180
   */
  rlon -= floor ((rlon + 180) / 360) * 360;

  out->lat = rlat;
  out->lon = rlon;
  return (0);
}

static int CPR_decode_surface (double ref_lat, double ref_lon,
                               int even_cprlat, int even_cprlon, int odd_cprlat, int odd_cprlon,
                               bool is_odd, pos_t *out)
{
  double air_dlat0 = 90.0 / 60.0;
  double air_dlat1 = 90.0 / 59.0;
  double lat0      = even_cprlat;
  double lat1      = odd_cprlat;
  double lon0      = even_cprlon;
  double lon1      = odd_cprlon;
  double rlon, rlat;

  /* Compute the Latitude Index "j"
   */
  int    j     = (int) floor (((59*lat0 - 60*lat1) / 131072) + 0.5);
  double rlat0 = air_dlat0 * (CPR_mod_func (j, 60) + lat0 / 131072);
  double rlat1 = air_dlat1 * (CPR_mod_func (j, 59) + lat1 / 131072);

  out->lat = out->lon = 0.0;
  CPR_error = 0;

  /*
   * Pick the quadrant that's closest to the reference location.
   * This is not necessarily the same quadrant that contains the
   * reference location.
   *
   * There are also only two valid quadrants: -90..0 and 0..90;
   * no correct message would try to encoding a latitude in the
   * ranges -180..-90 and 90..180.
   *
   * If the computed latitude is more than 45 degrees north of
   * the reference latitude (using the northern hemisphere
   * solution), then the southern hemisphere solution will be
   * closer to the refernce latitude.
   *
   * e.g. ref_lat=0, rlat=44, use rlat=44
   *      ref_lat=0, rlat=46, use rlat=46-90 = -44
   *      ref_lat=40, rlat=84, use rlat=84
   *      ref_lat=40, rlat=86, use rlat=86-90 = -4
   *      ref_lat=-40, rlat=4, use rlat=4
   *      ref_lat=-40, rlat=6, use rlat=6-90 = -84
   *
   * As a special case, -90, 0 and +90 all encode to zero, so
   * there's a little extra work to do there.
   */

  if (rlat0 == 0)
  {
    if (ref_lat < -45)
         rlat0 = -90;
    else if (ref_lat > 45)
         rlat0 = 90;
  }
  else if ((rlat0 - ref_lat) > 45)
  {
    rlat0 -= 90;
  }

  if (rlat1 == 0)
  {
    if (ref_lat < -45)
         rlat1 = -90;
    else if (ref_lat > 45)
         rlat1 = 90;
  }
  else if ((rlat1 - ref_lat) > 45)
  {
    rlat1 -= 90;
  }

  /* Check to see that the latitude is in range: -90 .. +90
   */
  if (rlat0 < -90 || rlat0 > 90 || rlat1 < -90 || rlat1 > 90)
  {
    CPR_SET_ERR (EINVAL);
    return (-2);       /* bad data */
  }

  /* Check that both are in the same latitude zone, or abort.
   */
  if (CPR_NL_func(rlat0) != CPR_NL_func(rlat1))
  {
    CPR_SET_ERR (ERANGE);
    return (-1);    /* positions crossed a latitude zone, try again later */
  }

  /* Compute ni and the Longitude Index "m"
   */
  if (is_odd)
  {
    int ni = CPR_N_func (rlat1, true);
    int m = (int) floor ((((lon0 * (CPR_NL_func(rlat1)-1)) -
                         (lon1 * CPR_NL_func(rlat1))) / 131072.0) + 0.5);

    rlon = CPR_Dlong_func2 (rlat1, true, true) * (CPR_mod_func (m, ni) + lon1/131072);
    rlat = rlat1;
  }
  else
  {
    int ni = CPR_N_func (rlat0, false);
    int m = (int) floor ((((lon0 * (CPR_NL_func(rlat0)-1)) -
                         (lon1 * CPR_NL_func(rlat0))) / 131072) + 0.5);

    rlon = CPR_Dlong_func2 (rlat0, false, true) * (CPR_mod_func(m, ni) + lon0/131072);
    rlat = rlat0;
  }

  /* Pick the quadrant that's closest to the reference location -
   * this is not necessarily the same quadrant that contains the
   * reference location. Unlike the latitude case, all four
   * quadrants are valid.
   *
   * if ref_lon is more than 45 degrees away, move some multiple of 90 degrees towards it
   */
  rlon += floor ((ref_lon - rlon + 45) / 90 ) * 90;  /* this might move us outside (-180..+180), we fix this below */

  /* Renormalize to -180 .. +180
   */
  rlon -= floor ((rlon + 180) / 360 ) * 360;

  out->lat = rlat;
  out->lon = rlon;
  return (0);
}

/*
 * This algorithm comes from:
 * 1090-WP29-07-Draft_CPR101 (which also defines decodeCPR() )
 *
 * Despite what the earlier comment here said, we should *not* be using trunc().
 * See Figure 5-5 / 5-6 and note that floor is applied to (0.5 + fRP - fEP), not
 * directly to (fRP - fEP). Eq 38 is correct.
 */
static int CPR_decode_relative (double ref_lat, double ref_lon, int cprlat, int cprlon,
                                bool is_odd, bool surface, pos_t *out)
{
  double air_dlat;
  double air_dlon;
  double fractional_lat = cprlat / 131072.0;
  double fractional_lon = cprlon / 131072.0;
  double rlon, rlat;
  int    j, m;

  out->lat = out->lon = 0.0;
  CPR_error = 0;

  air_dlat = (surface ? 90.0 : 360.0) / (is_odd ? 59.0 : 60.0);

  /* Compute the Latitude Index "j"
   */
  j = (int) (floor (ref_lat/air_dlat) +
             floor (0.5 + CPR_mod_double(ref_lat, air_dlat)/air_dlat - fractional_lat));

  rlat = air_dlat * (j + fractional_lat);
  if (rlat >= 270)
     rlat -= 360;

  /* Check to see that the latitude is in range: -90 .. +90
   */
  if (rlat < -90 || rlat > 90)
  {
    CPR_SET_ERR (EINVAL);
    return (-1);                        /* Time to give up - Latitude error */
  }

  /* Check to see that answer is reasonable - ie no more than 1/2 cell away
   */
  if (fabs(rlat - ref_lat) > (air_dlat/2))
  {
    CPR_SET_ERR (E2BIG);
    return (-1);                        /* Time to give up - Latitude error */
  }

  /* Compute the Longitude Index "m"
   */
  air_dlon = CPR_Dlong_func2 (rlat, is_odd, surface);
  m = (int) (floor (ref_lon/air_dlon) +
             floor (0.5 + CPR_mod_double(ref_lon, air_dlon)/air_dlon - fractional_lon));

  rlon = air_dlon * (m + fractional_lon);
  if (rlon > 180)
     rlon -= 360;

  /* Check to see that answer is reasonable - ie no more than 1/2 cell away
   */
  if (fabs(rlon - ref_lon) > (air_dlon / 2))
  {
    CPR_SET_ERR (E2BIG);
    return (-1);                        /* Time to give up - Longitude error */
  }

  out->lat = rlat;
  out->lon = rlon;
  return (0);
}

/**
 * Return true if it's OK for the aircraft to have travelled
 * from its last known position at `a->position.lat, a->position.lon`
 * to a new position at `pos->lat, pos->lon`
 * in a period of `now - a->seen_pos` msec.
 */
static bool CPR_speed_check (aircraft            *a,
                             const modeS_message *mm,
                             const pos_t         *pos,
                             uint64_t             now,
                             bool                 surface)
{
  uint64_t elapsed;
  double   max_dist, distance, elapsed_sec, speed_Ms;
  double   speed;          /* in knots */
  bool     dist_ok;

  if (!(a->AC_flags & MODES_ACFLAGS_LATLON_VALID))
     return (true);              /* no reference, assume OK */

  elapsed = now - a->seen_pos;   /* milli-sec */

  if ((mm->AC_flags & MODES_ACFLAGS_SPEED_VALID) && (a->AC_flags & MODES_ACFLAGS_SPEED_VALID))
       speed = (mm->velocity + a->speed) / 2;       /* average */
  else if (mm->AC_flags & MODES_ACFLAGS_SPEED_VALID)
       speed = mm->velocity;
  else if ((a->AC_flags & MODES_ACFLAGS_SPEED_VALID) && (now - a->seen_speed) < 30000)
       speed = a->speed;
  else speed = surface ? 100.0 : 600.0;   /* A guess; in knots */

  /* Work out a reasonable speed to use:
   *   current speed + 1/3
   *   surface speed min 20kt, max 150kt
   *   airborne speed min 200kt, no max
   */
  speed = speed * 4 / 3;
  if (surface)
  {
    if (speed < 20.0)
        speed = 20.0;
    if (speed > 150.0)
        speed = 150.0;
  }
  else
  {
    if (speed < 200.0)
        speed = 200.0;
  }

  /* 100m (surface) or 500m (airborne) base distance to allow for minor errors,
   * plus distance covered at the given speed for the elapsed time + 1 second.
   */
  if (surface)
       max_dist = 100.0;
  else max_dist = 500.0;

  elapsed_sec = (elapsed + 1000.0) / 1000.0;
  speed_Ms    = (speed * 1852.0) / 3600.0;     /* Knots -> meters/sec */

  max_dist += elapsed_sec * speed_Ms;

  /* find actual distance between old and new point
   */
  distance = geo_great_circle_dist (&a->position, pos);
  dist_ok  = (distance <= max_dist);

  if (!dist_ok)
  {
    a->seen_pos_EST = 0;
    CPR_TRACE ("%06X: speed check failed, %.1f sec, speed_Ms %.1f M/s, max_dist %.1f km, actual %.1f km\n",
               a->addr, elapsed_sec, speed_Ms, max_dist / 1000.0, distance / 1000.0);
  }
  else
  {
    a->distance    = distance;
    a->distance_ok = true;
  }
  return (dist_ok);
}

/**
 * CPR tests
 */
#undef  SMALL_VAL
#define SMALL_VAL 1E-6
#define FAIL_LAT  "              lat %.6f, expected %11.6f\n"
#define FAIL_LON  "              lon %.6f, expected %11.6f\n"

typedef struct airborne_test {
        int    even_cprlat, even_cprlon;  /* input: raw CPR values, even message */
        int    odd_cprlat, odd_cprlon;    /* input: raw CPR values, odd message */
        double even_rlat, even_rlon;      /* verify: expected position from decoding with is_odd=false (even message is latest) */
        double odd_rlat, odd_rlon;        /* verify: expected position from decoding with is_odd=true (odd message is latest) */
      } airborne_test;

typedef struct surface_test {
        double ref_lat, ref_lon;          /* input: reference location for decoding */
        int    even_cprlat, even_cprlon;  /* input: raw CPR values, even message */
        int    odd_cprlat, odd_cprlon;    /* input: raw CPR values, odd message */
        double even_rlat, even_rlon;      /* verify: expected position from decoding with is_odd=false (even message is latest) */
        double odd_rlat, odd_rlon;        /* verify: expected position from decoding with is_odd=true (odd message is latest) */
      } surface_test;

typedef struct relative_test {
        double ref_lat, ref_lon;          /* input: reference location for decoding */
        int    cprlat, cprlon;            /* input: raw CPR values, even or odd message */
        bool   is_odd;                    /* input: is_odd in raw message */
        bool   surface;                   /* input: decode as air (false) or surface (true) position */
        double rlat, rlon;                /* verify: expected position */
      } relative_test;

static const airborne_test CPR_airborne_tests[] = {
     { 80536, 9432, 61720, 9192, 51.686646, 0.700156, 51.686763, 0.701294 },
     { 80534, 9413, 61714, 9144, 51.686554, 0.698745, 51.686484, 0.697632 },

     /** \todo: more positions, bad data */
  };

static const surface_test CPR_surface_tests[] = {
     /*
      * The real position received here was on the Cambridge (UK) airport at 52.209976N 0.176507E
      * We mess with the reference location to check that the right quadrant is used.
      *
      * longitude quadrants:
      */
     { 52.0, -180.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601-180.0, 52.209976,      0.176507-180.0 },
     { 52.0, -140.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601-180.0, 52.209976,      0.176507-180.0 },
     { 52.0, -130.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601- 90.0, 52.209976,      0.176507-90.0  },
     { 52.0,  -50.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601- 90.0, 52.209976,      0.176507-90.0  },
     { 52.0,  -40.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507       },
     { 52.0,  -10.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507       },
     { 52.0,    0.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507       },
     { 52.0,   10.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507       },
     { 52.0,   40.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507       },
     { 52.0,   50.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601+90.0,  52.209976,      0.176507+90.0  },
     { 52.0,  130.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601+90.0,  52.209976,      0.176507+90.0  },
     { 52.0,  140.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601-180.0, 52.209976,      0.176507-180.0 },
     { 52.0,  180.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601-180.0, 52.209976,      0.176507-180.0 },

     /* latitude quadrants (but only 2). The decoded longitude also changes because the cell size changes with latitude
      */
     {  90.0,   0.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507 },
     {  52.0,   0.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507 },
     {   8.0,   0.0, 105730, 9259, 29693, 8997, 52.209984,      0.176601,       52.209976,      0.176507 },
     {   7.0,   0.0, 105730, 9259, 29693, 8997, 52.209984-90.0, 0.135269,       52.209976-90.0, 0.134299 },
     { -52.0,   0.0, 105730, 9259, 29693, 8997, 52.209984-90.0, 0.135269,       52.209976-90.0, 0.134299 },
     { -90.0,   0.0, 105730, 9259, 29693, 8997, 52.209984-90.0, 0.135269,       52.209976-90.0, 0.134299 },

     /* poles/equator cases:
      */
     { -46.0, -180.0, 0,      0,    0,     0,   -90.0,           -180.000000,    -90.0,         -180.0 },   /* south pole */
     { -44.0, -180.0, 0,      0,    0,     0,     0.0,           -180.000000,      0.0,         -180.0 },   /* equator */
     {  44.0, -180.0, 0,      0,    0,     0,     0.0,           -180.000000,      0.0,         -180.0 },   /* equator */
     {  46.0, -180.0, 0,      0,    0,     0,    90.0,           -180.000000,     90.0,         -180.0 },   /* north pole */
   };

static const relative_test CPR_relative_tests[] = {
       /*
        * AIRBORNE
        */
       { 52.0,  0.0,  80536, 9432, false, false, 51.686646, 0.700156 },   /* even, airborne */
       { 52.0,  0.0,  61720, 9192, true,  false, 51.686763, 0.701294 },   /* odd, airborne */
       { 52.0,  0.0,  80534, 9413, false, false, 51.686554, 0.698745 },   /* even, airborne */
       { 52.0,  0.0,  61714, 9144, true,  false, 51.686484, 0.697632 },   /* odd, airborne */

       /* test moving the receiver around a bit
        * We cannot move it more than 1/2 cell away before ambiguity happens.
        *
        * latitude must be within about 3 degrees (cell size is 360/60 = 6 degrees)
        */
       { 48.7,  0.0,  80536, 9432, false, false, 51.686646, 0.700156 },  /* even, airborne */
       { 48.7,  0.0,  61720, 9192, true,  false, 51.686763, 0.701294 },  /* odd, airborne */
       { 48.7,  0.0,  80534, 9413, false, false, 51.686554, 0.698745 },  /* even, airborne */
       { 48.7,  0.0,  61714, 9144, true,  false, 51.686484, 0.697632 },  /* odd, airborne */
       { 54.6,  0.0,  80536, 9432, false, false, 51.686646, 0.700156 },  /* even, airborne */
       { 54.6,  0.0,  61720, 9192, true,  false, 51.686763, 0.701294 },  /* odd, airborne */
       { 54.6,  0.0,  80534, 9413, false, false, 51.686554, 0.698745 },  /* even, airborne */
       { 54.6,  0.0,  61714, 9144, true,  false, 51.686484, 0.697632 },  /* odd, airborne */

       /* longitude must be within about 4.8 degrees at this latitude
        */
       { 52.0,  5.4,  80536, 9432, false, false, 51.686646, 0.700156 },  /* even, airborne */
       { 52.0,  5.4,  61720, 9192, true,  false, 51.686763, 0.701294 },  /* odd, airborne */
       { 52.0,  5.4,  80534, 9413, false, false, 51.686554, 0.698745 },  /* even, airborne */
       { 52.0,  5.4,  61714, 9144, true,  false, 51.686484, 0.697632 },  /* odd, airborne */
       { 52.0, -4.1,  80536, 9432, false, false, 51.686646, 0.700156 },  /* even, airborne */
       { 52.0, -4.1,  61720, 9192, true,  false, 51.686763, 0.701294 },  /* odd, airborne */
       { 52.0, -4.1,  80534, 9413, false, false, 51.686554, 0.698745 },  /* even, airborne */
       { 52.0, -4.1,  61714, 9144, true,  false, 51.686484, 0.697632 },  /* odd, airborne */

       /*
        * SURFACE
        *
        * Surface position on the Cambridge (UK) airport apron at 52.21N 0.18E
        */
       { 52.00,  0.00, 105730, 9259, false, true,  52.209984, 0.176601 },  /* even, surface */
       { 52.00,  0.00,  29693, 8997, true,  true,  52.209976, 0.176507 },  /* odd, surface */

       /* test moving the receiver around a bit
        * We cannot move it more than 1/2 cell away before ambiguity happens.
        *
        * latitude must be within about 0.75 degrees (cell size is 90/60 = 1.5 degrees)
        */
       { 51.46,  0.00, 105730, 9259, false, true,  52.209984, 0.176601 },  /* even, surface */
       { 51.46,  0.00,  29693, 8997, true,  true,  52.209976, 0.176507 },  /* odd, surface */
       { 52.95,  0.00, 105730, 9259, false, true,  52.209984, 0.176601 },  /* even, surface */
       { 52.95,  0.00,  29693, 8997, true,  true,  52.209976, 0.176507 },  /* odd, surface */

       /* longitude must be within about 1.25 degrees at this latitude
        */
       { 52.00,  1.40, 105730, 9259, false, true,  52.209984, 0.176601 },   /* even, surface */
       { 52.00,  1.40,  29693, 8997, true,  true,  52.209976, 0.176507 },   /* odd, surface */
       { 52.00, -1.05, 105730, 9259, false, true,  52.209984, 0.176601 },   /* even, surface */
       { 52.00, -1.05,  29693, 8997, true,  true,  52.209976, 0.176507 },   /* odd, surface */
     };

static bool CPR_airborne_test (void)
{
  unsigned i, err = 0;
  const airborne_test *t = CPR_airborne_tests + 0;

  printf ("%s():\n", __FUNCTION__);

  for (i = 0; i < DIM(CPR_airborne_tests); i++, t++)
  {
    int   res;
    pos_t pos = { 0.0, 0.0 };

    printf ("  [%2u, EVEN]: ", i);

    res = CPR_decode_airborne (t->even_cprlat, t->even_cprlon, t->odd_cprlat, t->odd_cprlon,
                               false, &pos);

    if (res || fabs(pos.lat - t->even_rlat) > SMALL_VAL || fabs(pos.lon - t->even_rlon) > SMALL_VAL)
    {
      err++;
      printf ("FAIL: %s, (%d,%d,%d,%d,EVEN):\n" FAIL_LAT FAIL_LON,
              CPR_strerror(), t->even_cprlat, t->even_cprlon, t->odd_cprlat, t->odd_cprlon,
              pos.lat, t->even_rlat, pos.lon, t->even_rlon);
    }
    else
      puts ("PASS");

    printf ("  [%2u, ODD]:  ", i);
    pos.lat = pos.lon = 0.0;

    res = CPR_decode_airborne (t->even_cprlat, t->even_cprlon, t->odd_cprlat, t->odd_cprlon,
                               true, &pos);

    if (res || fabs(pos.lat - t->odd_rlat) > SMALL_VAL || fabs(pos.lon - t->odd_rlon) > SMALL_VAL)
    {
      err++;
      printf ("FAIL: %s, (%d,%d,%d,%d,ODD):\n" FAIL_LAT FAIL_LON,
              CPR_strerror(), t->even_cprlat, t->even_cprlon, t->odd_cprlat, t->odd_cprlon,
              pos.lat, t->odd_rlat, pos.lon, t->odd_rlon);
    }
    else
      puts ("PASS");
  }
  puts ("");
  return (err == 0);
}

static bool CPR_surface_test (void)
{
  unsigned i, err = 0;
  const surface_test *t = CPR_surface_tests + 0;

  printf ("%s():\n", __FUNCTION__);

  for (i = 0; i < DIM(CPR_surface_tests); i++, t++)
  {
    int   res;
    pos_t pos = { 0.0, 0.0 };

    printf ("  [%2u, EVEN]: ", i);
    res = CPR_decode_surface (t->ref_lat, t->ref_lon, t->even_cprlat, t->even_cprlon,
                              t->odd_cprlat, t->odd_cprlon, false, &pos);

    if (res || fabs(pos.lat - t->even_rlat) > SMALL_VAL || fabs(pos.lon - t->even_rlon) > SMALL_VAL)
    {
      err++;
      printf ("FAIL: %s (%.6f,%.6f,%d,%d,%d,%d,EVEN):\n" FAIL_LAT FAIL_LON,
              CPR_strerror(), t->ref_lat, t->ref_lon, t->even_cprlat, t->even_cprlon,
              t->odd_cprlat, t->odd_cprlon, pos.lat, t->even_rlat,
              pos.lon, t->even_rlon);
    }
    else
      puts ("PASS");

    printf ("  [%2u, ODD]:  ", i);
    pos.lat = pos.lon = 0.0;
    res = CPR_decode_surface (t->ref_lat, t->ref_lon, t->even_cprlat, t->even_cprlon,
                              t->odd_cprlat, t->odd_cprlon, true, &pos);

    if (res || fabs(pos.lat - t->odd_rlat) > SMALL_VAL || fabs(pos.lon - t->odd_rlon) > SMALL_VAL)
    {
      err++;
      printf ("FAIL: %s (%.6f,%.6f,%d,%d,%d,%d,ODD):\n" FAIL_LAT FAIL_LON,
              CPR_strerror(), t->ref_lat, t->ref_lon, t->even_cprlat, t->even_cprlon,
              t->odd_cprlat, t->odd_cprlon, pos.lat, t->odd_rlat,
              pos.lon, t->odd_rlon);
    }
    else
      puts ("PASS");
  }
  puts ("");
  return (err == 0);
}

static bool CPR_relative_test (void)
{
  unsigned i, err = 0;
  const relative_test *t = CPR_relative_tests + 0;

  printf ("%s():\n", __FUNCTION__);

  for (i = 0; i < DIM(CPR_relative_tests); i++, t++)
  {
    int   res;
    pos_t pos = { 0.0, 0.0 };

    printf ("  [%2u]: ", i);
    res = CPR_decode_relative (t->ref_lat, t->ref_lon, t->cprlat, t->cprlon,
                               t->is_odd, t->surface, &pos);

    if (res || fabs(pos.lat - t->rlat) > SMALL_VAL || fabs(pos.lon - t->rlon) > SMALL_VAL)
    {
      err++;
      printf ("FAIL: %s, (%.6f,%.6f,%d,%d,%d,%d) failed:\n" FAIL_LAT FAIL_LON,
              CPR_strerror(), t->ref_lat, t->ref_lon, t->cprlat, t->cprlon, t->is_odd, t->surface,
              pos.lat, t->rlat, pos.lon, t->rlon);
    }
    else
      puts ("PASS");
  }
  puts ("");
  return (err == 0);
}

bool cpr_do_tests (void)
{
  bool ok = true;

  puts ("");
  ok = CPR_airborne_test() && ok;
  ok = CPR_surface_test()  && ok;
  ok = CPR_relative_test() && ok;
  return (ok);
}

