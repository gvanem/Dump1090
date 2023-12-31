/**\file    airports.h
 * \ingroup Main
 */
#ifndef _AIRPORTS_H
#define _AIRPORTS_H

#include "aircraft.h"

/**
 * \def AIRPORT_DATABASE_CSV
 * Our default airport-database relative to `Modes.where_am_I`.
 */
#define AIRPORT_DATABASE_CSV   "airport-codes.csv"

/**
 * \def AIRPORT_DATABASE_CACHE
 * Our airport API cache in the `%TEMP%\\dump1090` directory.
 */
#define AIRPORT_DATABASE_CACHE  "airport-api-cache.csv"

/**
 * \def AIRPORT_FREQ_CSV
 * Our airport-frequency database relative to `Modes.where_am_I`.
 */
#define AIRPORT_FREQ_CSV  "airport-frequencies.csv"

uint32_t airports_init (void);
uint32_t airports_rc (void);
void     airports_exit (bool free_airports);
void     airports_show_stats (void);
void     airports_background (uint64_t now);
bool     airports_update_CSV (const char *file);

bool     airports_API_get_flight_info (const char *call_sign, uint32_t addr,
                                       const char **departure, const char **destination);
bool     airports_API_flight_log_entering (const aircraft *a);
bool     airports_API_flight_log_resolved (const aircraft *a);
bool     airports_API_flight_log_leaving (const aircraft *a);

#endif /* _AIRPORTS_H */
