/**\file    demod-2000.h
 * \ingroup Samplers
 * \brief   Demodulator for 2 MS/s decoding
 */
#ifndef DEMOD_2000_H
#define DEMOD_2000_H

#include <stdint.h>

uint32_t demodulate_2000 (uint16_t *m, uint32_t mlen);

#endif
