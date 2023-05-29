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

/**
 * \enum airport_t
 * The source type for an \ref airport or \ref flight_info record.
 */
typedef enum airport_t {
        AIRPORT_CSV = 1,
        AIRPORT_API_LIVE,
        AIRPORT_API_CACHED
      } airport_t;

/**
 * \typedef airport
 *
 * Describes an airport. Data can be from these sources:
 *  \li A .CSV-file         (`Mode.airport_db == "airport-codes.csv"`)
 *  \li A live API request.
 *  \li A cached API request (`Mode.airport_api_cache == "%TEMP%\\airport-api-cache.csv"`).
 *
 * These are NOT on the same order as in `airport-codes.csv`. <br>
 * CSV header:
 * `# ICAO, IATA, Full_name, Continent, Location, Longitude, Latitude`
 */
typedef struct airport {
        char            ICAO [10];       // ICAO code
        char            IATA [10];       // IATA code
        char            continent [ 3];  // ISO-3166 2 letter continent code
        char            location  [30];  // location or city
        char            full_name [50];  // Full name
        pos_t           pos;             // latitude & longitude
        airport_t       type;            // source of this record
        struct airport *next;            // next airport in our linked list
      } airport;

bool     airports_init (void);
void     airports_exit (bool free_airports);
bool     airports_update_CSV (const char *file);
uint32_t airports_numbers_CSV (void);
uint32_t airports_numbers_API (void);

/**
 * \typedef airport_freq
 *
 * Data for a single airport frequency.
 * Also contains a link to a `airport*` node.
 */
typedef struct airport_freq {
        char           freq_id [3];
        char           ident [10];
        double         frequency;
        const airport *airport;
      } airport_freq;

/*
 * Handling of "Flight Information".
 */

/**
 * \typedef flight_info
 *
 * A flight-information record from a live or cached API reequest.
 */
typedef struct flight_info {
        char                callsign    [10]; /**< callsign for this flight */
        char                departure   [30]; /**< departure airport for this flight */
        char                destination [30]; /**< destination airport for this flight */
        airport_t           type;             /**< type: `AIRPORT_API_LIVE` or `AIRPORT_API_CACHED` */
        uint64_t            created;          /**< Tick-time (in milli-sec) at which this record was created */
        struct flight_info *next;             /**< next flight in our linked list */
      } flight_info;

flight_info *flight_info_get (const char *callsign);

#endif /* _AIRPORTS_H */
