/**\file    raw-sbs.h
 * \ingroup Decoder
 */
#pragma once

#include "misc.h"

bool raw_decode_message (mg_iobuf *msg, int loop_cnt);
bool sbs_decode_message (mg_iobuf *msg, int loop_cnt);

void raw_out_send (const modeS_message *mm);
void sbs_out_send (const modeS_message *mm);

bool raw_in_set_host_port (const char *arg);
bool raw_out_set_host_port (const char *arg);

bool raw_in_set_port (const char *arg);
bool raw_out_set_port (const char *arg);

bool sbs_out_set_port (const char *arg);
bool sbs_in_set_host_port (const char *arg);

void raw_in_stats (void);
void sbs_in_stats (void);
