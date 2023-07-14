/**\file    airports.h
 * \ingroup Main
 */
#ifndef _AIRPORTS_H
#define _AIRPORTS_H

#include "misc.h"

/**
 * \def AIRPORT_DATABASE_CSV
 * Our default airport-database relative to `Modes.where_am_I`.
 */
#define AIRPORT_DATABASE_CSV   "airport-codes.csv"

/**
 * \def AIRPORT_DATABASE_CACHE
 * Our airport API cache in the `%TEMP%` directory.
 */
#define AIRPORT_DATABASE_CACHE  "airport-api-cache.csv"

/**
 * \def AIRPORT_FREQ_CSV
 * Our airport-frequency database relative to `Modes.where_am_I`.
 */
#define AIRPORT_FREQ_CSV  "airport-frequencies.csv"

uint32_t airports_init (void);
void     airports_exit (void);
void     airports_show_stats (void);
bool     airports_update_CSV (const char *file);
bool     airports_API_get_flight_info (const char *call_sign, const char **departure, const char **destination);
void     airports_API_show_stats (uint64_t now);
void     airports_API_remove_stale (uint64_t now);

#endif /* _AIRPORTS_H */
