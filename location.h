/**\file    location.h
 * \ingroup Misc
 *
 * Simple async 'Windows Location API' interface.
 */
#ifndef _LOCATION_H
#define _LOCATION_H

#include <stdbool.h>

struct pos_t;

void location_exit (void);
bool location_poll (struct pos_t *pos);
bool location_get_async (void);

#endif
