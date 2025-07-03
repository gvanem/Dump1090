/**\file    crc.h
 * \ingroup Misc
 * \brief   Mode S CRC calculation and error correction.
 *
 * Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
 */
#pragma once

/* Global max for fixable bit erros
 */
#define MODES_MAX_BITERRORS 2

typedef struct errorinfo {
        uint32_t syndrome;                  // CRC syndrome
        int      errors;                    // number of errors
        int8_t   bit [MODES_MAX_BITERRORS]; // bit positions to fix (-1 = no bit)
      } errorinfo;

void       crc_init (int fix_bits);
void       crc_exit (void);
uint32_t   crc_checksum (const uint8_t *msg, int bitlen);
errorinfo *crc_checksum_diagnose (uint32_t syndrome, int bitlen);
void       crc_checksum_fix (uint8_t *msg, const errorinfo *info);
