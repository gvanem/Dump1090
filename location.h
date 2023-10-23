/**\file    location.h
 * \ingroup Misc
 *
 * Simple async 'Windows Location API' interface.
 */
#pragma once

#include <stdbool.h>

void location_exit (void);
bool location_poll (pos_t *pos);
bool location_get_async (void);
