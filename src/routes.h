/**\file    routes.h
 * \ingroup Main
 *
 * Lookup of generated routes[] data.
 */
#pragma once

typedef struct route_record {
        char  call_sign    [10];  /**< Call-sign for this route (or flight) */
        char  departure    [10];  /**< ICAO departure airport for this route */
        char  destination  [10];  /**< Final ICAO destination airport for this route */
        char  stop_over [5][10];  /**< 5 possible stop-over airports. Or "" for none */
      } route_record;

extern const route_record route_records[];
extern size_t             route_records_num;
