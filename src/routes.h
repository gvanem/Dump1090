/**\file    routes.h
 * \ingroup Main
 *
 * Lookup of generated `*route_records` data.
 */
#pragma once

#if defined(USE_BIN_FILES)
  #include <stdint.h>
  #include <time.h>
  #include <gen_data.h>

  #pragma pack(push, 1)

  /*
   * Copied from '$(TMP)/dump1090/standing-data/results/gen_data.h'
   */
  typedef struct route_record2 {     /* matching 'routes_format = "<8s20s"' */
          char  call_sign [8];

          /* this is really `airports[20]`. Like "EGCC-LTBS".
           * Or "KCLT-KRSW-KCLT" with one stop-over airport.
           * Or "KJFK-EBBR-ZSYT-RKSI" with two stop-over airport.
           * Airport names are always 4 letter ICAO.
           */
          char  departure   [10];
          char  destination [10];
        } route_record2;

  /*
   * Copied from '$(TMP)/dump1090/standing-data/results/test-routes.c'
   */
  typedef struct BIN_header {
          char     bin_marker [12];   /* BIN-file marker == "BIN-dump1090" */
          time_t   created;           /* time of creation (64-bits) */
          uint32_t rec_num;           /* number of records in .BIN-file == 534513 */
          uint32_t rec_len;           /* sizeof(record) in .BIN-file == 28 */
        } BIN_header;

  #pragma pack(pop)
#endif
