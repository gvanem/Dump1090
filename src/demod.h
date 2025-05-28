/**\file    demod.h
 * \ingroup Demodulators
 * \brief   Demodulators for 2, 2.4 and 8.0 MS/s decoding
 */
#pragma once

void demod_2000 (const mag_buf *mag);
void demod_2400 (const mag_buf *mag);
void demod_2400_AC (const mag_buf *mag);
void demod_8000 (const mag_buf *mag);
int  demod_8000_alloc (void);
void demod_8000_free (void);
