/**\file    speech.h
 * \ingroup Misc
 *
 * Simple SAPI5 speech-interface for Dump1090.
 */
#ifndef _SPEECH_H
#define _SPEECH_H

#include "misc.h"

bool speak_init (int voice, int volume);
void speak_exit (void);
bool speak_stringA (const char *str);
bool speak_stringW (const wchar_t *str);
bool speak_poll (void);

#endif  /* _SPEECH_H */

