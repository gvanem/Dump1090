/**\file    cpr.h
 * \ingroup Misc
 */
#ifndef _CPR_H
#define _CPR_H

struct aircraft;

bool cpr_decode (struct aircraft *a, bool is_odd);

bool cpr_decode_airborne (int even_cprlat, int even_cprlon, int odd_cprlat, int odd_cprlon,
                          bool is_odd, double *out_lat, double *out_lon);

bool cpr_decode_surface  (double ref_lat, double ref_lon, int even_cprlat, int even_cprlon, int odd_cprlat, int odd_cprlon,
                          bool is_odd, double *out_lat, double *out_lon);

bool cpr_decode_relative (double ref_lat, double ref_lon, int cprlat, int cprlon,
                          bool is_odd, bool surface, double *out_lat, double *out_lon);

bool cpr_tests (void);

#endif /* _CPR_H */
