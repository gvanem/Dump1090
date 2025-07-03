/**\file    speech.h
 * \ingroup Misc
 *
 * Simple SAPI5 speech-interface for Dump1090.
 */
#pragma once

#include "misc.h"

bool speak_init (int voice, int volume);
void speak_exit (void);
bool speak_string (_Printf_format_string_ const char *fmt, ...) ATTR_PRINTF(1, 2);
