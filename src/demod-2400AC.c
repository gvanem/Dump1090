/*
 * Part of dump1090, a Mode S message decoder for RTLSDR / SDRPlay.
 *
 * Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
 */

/**\file    demod-2400AC.c
 * \ingroup Demodulators
 * \brief   2.4 MHz Mode-AC detection and decoding.
 * \note    Not used t the moment
 */
#include "misc.h"
#include "demod.h"

/*
 * Mode A/C bits are 1.45us wide, consisting of 0.45us on and 1.0us off
 * We track this in terms of a (virtual) 60MHz clock, which is the lowest common multiple
 * of the bit frequency and the 2.4MHz sampling frequency
 *
 *            0.45us = 27 cycles }
 *            1.00us = 60 cycles } one bit period = 1.45us = 87 cycles
 *
 * one 2.4MHz sample = 25 cycles
 */
void demod_2400_AC (const mag_buf *mag)
{
  struct modeS_message mm;
  const uint16_t *m = mag->data;
  uint32_t        mlen = mag->valid_length - mag->overlap;
  uint32_t        f1_sample;

  memset (&mm, 0, sizeof(mm));

  double   noise_stddev = sqrt (mag->mean_power - mag->mean_level * mag->mean_level);
  unsigned noise_level = (unsigned) ((mag->mean_power + noise_stddev) * 65535 + 0.5);

  for (f1_sample = 1; f1_sample < mlen; f1_sample++)
  {
     /*
      * Mode A/C messages should match this bit sequence:
      *
      * bit #     value
      *   -1       0    quiet zone
      *    0       1    framing pulse (F1)
      *    1      C1
      *    2      A1
      *    3      C2
      *    4      A2
      *    5      C4
      *    6      A4
      *    7       0    quiet zone (X1)
      *    8      B1
      *    9      D1
      *   10      B2
      *   11      D2
      *   12      B4
      *   13      D4
      *   14       1    framing pulse (F2)
      *   15       0    quiet zone (X2)
      *   16       0    quiet zone (X3)
      *   17     SPI
      *   18       0    quiet zone (X4)
      *   19       0    quiet zone (X5)
      *
      * Look for a F1 and F2 pair,
      * with F1 starting at offset f1_sample.
      *
      * the first framing pulse covers 3.5 samples:
      *
      * |----|        |----|
      * | F1 |________| C1 |_
      *
      * | 0 | 1 | 2 | 3 | 4 |
      *
      * and there is some unknown phase offset of the
      * leading edge e.g.:
      *
      *   |----|        |----|
      * __| F1 |________| C1 |_
      *
      * | 0 | 1 | 2 | 3 | 4 |
      *
      * in theory the "on" period can straddle 3 samples
      * but it's not a big deal as at most 4% of the power
      * is in the third sample.
      */

     if (!(m[f1_sample-1] < m[f1_sample+0]))
        continue;      /* not a rising edge */

     if (m[f1_sample+2] > m[f1_sample+0] || m[f1_sample+2] > m[f1_sample+1])
         continue;      /* quiet part of bit wasn't sufficiently quiet */

     unsigned f1_level = (m[f1_sample+0] + m[f1_sample+1]) / 2;

     if (noise_level * 2 > f1_level)
     {
       /* require 6dB above noise */
       continue;
     }

     /* Estimate initial clock phase based on the amount of power
      * that ended up in the second sample
      */
     float    f1a_power = (float)m[f1_sample] * m[f1_sample];
     float    f1b_power = (float)m[f1_sample+1] * m[f1_sample+1];
     float    fraction  = f1b_power / (f1a_power + f1b_power);
     unsigned f1_clock  = (unsigned) (25 * (f1_sample + fraction * fraction) + 0.5);

     /* same again for F2:
      * F2 is 20.3us / 14 bit periods after F1
      */
     unsigned f2_clock = f1_clock + (87 * 14);
     unsigned f2_sample = f2_clock / 25;

     assert(f2_sample < mlen + mag->overlap);

     if (!(m[f2_sample-1] < m[f2_sample+0]))
        continue;

     if (m[f2_sample+2] > m[f2_sample+0] || m[f2_sample+2] > m[f2_sample+1])
         continue;      /* quiet part of bit wasn't sufficiently quiet */

     unsigned f2_level = (m[f2_sample+0] + m[f2_sample+1]) / 2;

     if (noise_level * 2 > f2_level)
     {
       /* require 6dB above noise */
       continue;
     }

     unsigned f1f2_level = (f1_level > f2_level ? f1_level : f2_level);
     float    midpoint = sqrtf (noise_level * f1f2_level); /* geometric mean of the two levels */
     unsigned signal_threshold = (unsigned) (midpoint * M_SQRT2 + 0.5);  /* +3dB */
     unsigned noise_threshold  = (unsigned) (midpoint / M_SQRT2 + 0.5);  /* -3dB */

     /* Looks like a real signal. Demodulate all the bits
      */
     unsigned uncertain_bits = 0;
     unsigned noisy_bits = 0;
     unsigned bits = 0;
     unsigned bit;
     unsigned clock;

     for (bit = 0, clock = f1_clock; bit < 20; ++bit, clock += 87)
     {
       unsigned sample = clock / 25;

       bits <<= 1;
       noisy_bits <<= 1;
       uncertain_bits <<= 1;

       /* check for excessive noise in the quiet period
        */
       if (m[sample+2] >= signal_threshold)
          noisy_bits |= 1;

       /* decide if this bit is on or off
        */
       if (m[sample+0] >= signal_threshold || m[sample+1] >= signal_threshold)
       {
         bits |= 1;
       }
       else if (m[sample+0] > noise_threshold && m[sample+1] > noise_threshold)
       {
         /* not certain about this bit */
         uncertain_bits |= 1;
       }
       else
       {
         /* this bit is off */
       }
     }

     /* framing bits must be on
      */
     if ((bits & 0x80020) != 0x80020)
        continue;

     /* quiet bits must be off
      */
     if ((bits & 0x0101B) != 0)
        continue;

     if (noisy_bits || uncertain_bits)
        continue;

     /* Convert to the form that we use elsewhere:
      *  00 A4 A2 A1  00 B4 B2 B1  SPI C4 C2 C1  00 D4 D2 D1
      */
     unsigned modeac =
         ((bits & 0x40000) ? 0x0010 : 0) |  /* C1 */
         ((bits & 0x20000) ? 0x1000 : 0) |  /* A1 */
         ((bits & 0x10000) ? 0x0020 : 0) |  /* C2 */
         ((bits & 0x08000) ? 0x2000 : 0) |  /* A2 */
         ((bits & 0x04000) ? 0x0040 : 0) |  /* C4 */
         ((bits & 0x02000) ? 0x4000 : 0) |  /* A4 */
         ((bits & 0x00800) ? 0x0100 : 0) |  /* B1 */
         ((bits & 0x00400) ? 0x0001 : 0) |  /* D1 */
         ((bits & 0x00200) ? 0x0200 : 0) |  /* B2 */
         ((bits & 0x00100) ? 0x0002 : 0) |  /* D2 */
         ((bits & 0x00080) ? 0x0400 : 0) |  /* B4 */
         ((bits & 0x00040) ? 0x0004 : 0) |  /* D4 */
         ((bits & 0x00004) ? 0x0080 : 0);   /* SPI */

      /* This message looks good, submit it */

      /* For consistency with how the Beast / Radarcape does it,
       * we report the timestamp at the second framing pulse (F2)
       */
      mm.timestamp_msg = mag->sample_timestamp + f2_clock / 5;  /* 60MHz -> 12MHz */

      /* compute message receive time as block-start-time + difference in the 12MHz clock
       */
      mm.sys_timestamp_msg = mag->sys_timestamp + receiveclock_ms_elapsed (mag->sample_timestamp, mm.timestamp_msg);

      Modes.stat.valid_preamble++;
      Modes.stat.demod_modeac++;

      decode_mode_A_message (&mm, modeac);

      /* Pass data to the next layer
       */
      modeS_user_message (&mm);

      f1_sample += (20*87 / 25);
  }
}
