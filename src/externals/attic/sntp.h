/**\file    sntp.h
 * \ingroup Misc
 *
 * Simple Network Time Protocol functions for Dump1090.
 */
#pragma once

#include "misc.h"

#define SNTP_DEFAULT_SERVER  "time.google.com"
#define SNTP_TIMEOUT         4000
#define MG_EV_SNTP_TIMEOUT   (MG_EV_USER + 1)

void     sntp_init (bool testing);
void     sntp_exit (void);
void     sntp_send (int idx, mg_connection *c);
int      sntp_close (int idx, mg_connection *c, int event);
bool     sntp_handler (int idx, mg_connection *c);
void     sntp_resolve (int idx, bool resolved);
bool     sntp_poll (int idx);
uint64_t sntp_time (int idx);
int      sntp_timeout (int idx);
int      sntp_test (int idx);

