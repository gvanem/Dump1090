/**\file    location.h
 * \ingroup Misc
 *
 * Various Windows Location API
 */
#ifndef _LOCATION_H
#define _LOCATION_H

#include <stdbool.h>

bool location_init (void);
void location_exit (void);
bool location_poll (void);
bool location_get_async (void);

#endif
