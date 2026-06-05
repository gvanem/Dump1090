/**\file    airports.h
 * \ingroup Main
 */
#pragma once

#include "misc.h"

void        airports_pre_init (void);
uint32_t    airports_init (void);
uint32_t    airports_num (uint32_t *num);
void        airports_exit (bool free_airports);
void        airports_show_stats (void);
void        airports_background (uint64_t now);
bool        airports_update_CSV (const char *file);
bool        airports_set_csv (const char *arg);
bool        airports_set_url (const char *arg);
bool        airports_prefer_adsb_lol (const char *arg);
bool        airports_API_get_flight_info (const char *call_sign, uint32_t addr,
                                          const char **departure, const char **destination);
bool        airports_API_flight_log_entering (const aircraft *a);
bool        airports_API_flight_log_resolved (const aircraft *a);
bool        airports_API_flight_log_leaving (const aircraft *a);
const char *airport_find_location (const char *IATA_or_ICAO);
const char *airport_find_location_by_IATA (const char *IATA);
const char *airport_find_location_by_ICAO (const char *ICAO);
