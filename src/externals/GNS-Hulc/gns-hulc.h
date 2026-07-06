/**\file    gns-hulc.h
 * \ingroup GNS-HULC
 *
 * Stuff for GNS / HULC protocol:
 * serial functions for GNS Electronics' HULC -M smart antenna.
 */
#pragma once

#include "misc.h"

/**
 * \def GNS_HULC_DEFAULT_COMPORT
 * Default COM-port for `--device gns-hulc`
 */
#define GNS_HULC_DEFAULT_COMPORT  1

/**
 * \def GNS_HULC_SLEEP
 *
 * Sleep() time in `gns_hulc_read_loop()` if the packet-queue is empty.
 * Also the Sleep() time in `main_data_loop()` between polls.
 */
#define GNS_HULC_SLEEP 50

HANDLE   gns_hulc_init (uint16_t port);
void     gns_hulc_exit (HANDLE hnd);
void     gns_hulc_stats (void);
void     gns_hulc_tests (void);
bool     gns_hulc_set_beast (const char *arg);
bool     gns_hulc_set_baud (const char *arg);
bool     gns_hulc_set_port (const char *arg);
bool     gns_hulc_gps_enable (const char *arg);
bool     gns_hulc_gps_info (pos_t *pos, int *altitude, int *num, double *hdop);
bool     gns_hulc_set_buf_size (const char *arg);
bool     gns_hulc_set_poll_ms (const char *arg);
void     gns_hulc_read_loop (void);
void     gns_hulc_poll (void);

bool     gns_hulc_gps_detected (void);
bool     gns_hulc_gps_enabled (void);
uint64_t gns_hulc_junk (void);
uint64_t gns_hulc_too_short (void);
