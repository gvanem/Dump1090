/**\file    gns-hulc.h
 * \ingroup Devices
 *
 * Stuff for GNS / HULC protocol:
 * serial functions for GNS Electronics' HULC -M smart antenna.
 */
#pragma once

#include "misc.h"

extern HANDLE gns_hulc_init (uint16_t port, bool beast_enable);
extern int    gns_hulc_exit (HANDLE hnd);
extern int    gns_hulc_read (HANDLE hnd, char *buf, size_t len);
extern int    gns_hulc_write (HANDLE hnd, const char *buf, size_t len);
extern void   gns_hulc_hexdump (const unsigned char *buf, size_t len, const char *in_out, const char *file, unsigned line);

#define GNS_HULC_HEXDUMP(buf, len, in_out) \
        gns_hulc_hexdump ((const unsigned char*)(buf), (size_t)(len), in_out, __FILE__, __LINE__)
