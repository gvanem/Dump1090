/*
 * Part of dump1090, a Mode S message decoder for RTLSDR / SDRPlay.
 *
 * Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
 */

/**\file    demod-2400.c
 * \ingroup Demodulators
 * \brief   2.4 MHz Mode S detection and decoding.
 */
#include "misc.h"
#include "demod.h"

/**
 * 2.4 MHz sampling rate version
 *
 * When sampling at 2.4MHz we have exactly 6 samples per 5 symbols.
 * Each symbol is 500ns wide, each sample is 416.7ns wide
 *
 * We maintain a phase offset that is expressed in units of 1/5 of a sample i.e. 1/6 of a symbol, 83.333ns
 * Each symbol we process advances the phase offset by 6 i.e. 6/5 of a sample, 500ns
 *
 * The correlation functions below correlate a 1-0 pair of symbols (i.e. manchester encoded 1 bit)
 * starting at the given sample, and assuming that the symbol starts at a fixed 0-5 phase offset within
 * m[0]. They return a correlation value, generally interpreted as >0 = 1 bit, <0 = 0 bit
 *
 * TODO check if there are better (or more balanced) correlation functions to use here
 *
 * nb: the correlation functions sum to zero, so we do not need to adjust for the DC offset in the input signal
 * (adding any constant value to all of m[0..3] does not change the result)
 */
static inline int slice_phase0 (const uint16_t *m)
{
  return 5 * m[0] - 3 * m[1] - 2 * m[2];
}

static inline int slice_phase1 (const uint16_t *m)
{
  return 4 * m[0] - m[1] - 3 * m[2];
}

static inline int slice_phase2 (const uint16_t *m)
{
  return 3 * m[0] + m[1] - 4 * m[2];
}

static inline int slice_phase3 (const uint16_t *m)
{
  return 2 * m[0] + 3 * m[1] - 5 * m[2];
}

static inline int slice_phase4 (const uint16_t *m)
{
  return m[0] + 5 * m[1] - 5 * m[2] - m[3];
}

static inline int correlate_phase0 (const uint16_t *m)
{
  return slice_phase0(m) * 26;
}

static inline int correlate_phase1 (const uint16_t *m)
{
  return slice_phase1(m) * 38;
}

static inline int correlate_phase2 (const uint16_t *m)
{
  return slice_phase2(m) * 38;
}

static inline int correlate_phase3 (const uint16_t *m)
{
  return slice_phase3(m) * 26;
}

static inline int correlate_phase4 (const uint16_t *m)
{
  return slice_phase4(m) * 19;
}

/*
 * These functions work out the correlation quality for the 10 symbols (5 bits) starting at m[0] + given phase offset.
 * This is used to find the right phase offset to use for decoding.
 */
static inline int correlate_check_0 (const uint16_t *m)
{
  return abs (correlate_phase0(&m[0])) +
         abs (correlate_phase2(&m[2])) +
         abs (correlate_phase4(&m[4])) +
         abs (correlate_phase1(&m[7])) +
         abs (correlate_phase3(&m[9]));
}

static inline int correlate_check_1 (const uint16_t *m)
{
  return abs (correlate_phase1(&m[0])) +
         abs (correlate_phase3(&m[2])) +
         abs (correlate_phase0(&m[5])) +
         abs (correlate_phase2(&m[7])) +
         abs (correlate_phase4(&m[9]));
}

static inline int correlate_check_2 (const uint16_t *m)
{
  return abs (correlate_phase2(&m[0])) +
         abs (correlate_phase4(&m[2])) +
         abs (correlate_phase1(&m[5])) +
         abs (correlate_phase3(&m[7])) +
         abs (correlate_phase0(&m[10]));
}

static inline int correlate_check_3 (const uint16_t *m)
{
  return abs (correlate_phase3(&m[0])) +
         abs (correlate_phase0(&m[3])) +
         abs (correlate_phase2(&m[5])) +
         abs (correlate_phase4(&m[7])) +
         abs (correlate_phase1(&m[10]));
}

static inline int correlate_check_4 (const uint16_t *m)
{
  return abs (correlate_phase4(&m[0])) +
         abs (correlate_phase1(&m[3])) +
         abs (correlate_phase3(&m[5])) +
         abs (correlate_phase0(&m[8])) +
         abs (correlate_phase2(&m[10]));
}

/*
 * Work out the best phase offset to use for the given message.
 */
static int best_phase (const uint16_t *m)
{
  int test, best = -1;
  int bestval = (m[0] + m[1] + m[2] + m[3] + m[4] + m[5]); /* minimum correlation quality we will accept */

  /*
   * Empirical testing suggests that 4..8 is the best range to test for here
   * (testing a wider range runs the danger of picking the wrong phase for
   * a message that would otherwise be successfully decoded - the correlation
   * functions can match well with a one symbol / half bit offset).
   *
   * This is consistent with the peak detection which should produce
   * the first data symbol with phase offset 4..8
   */
  test = correlate_check_4 (&m[0]);
  if (test > bestval)
  {
    bestval = test;
    best = 4;
  }
  test = correlate_check_0 (&m[1]);
  if (test > bestval)
  {
    bestval = test;
    best = 5;
  }
  test = correlate_check_1(&m[1]);
  if (test > bestval)
  {
    bestval = test;
    best = 6;
  }
  test = correlate_check_2(&m[1]);
  if (test > bestval)
  {
    bestval = test;
    best = 7;
  }
  test = correlate_check_3(&m[1]);
  if (test > bestval)
  {
    bestval = test;
    best = 8;
  }
  return best;
}

/**
 * Given `mlen` magnitude samples in `mag`, sampled at 2.4 MHz,
 * try to demodulate some Mode S messages.
 */
void demod_2400 (const mag_buf *mag)
{
  modeS_message   mm;
  uint8_t         msg1 [MODES_LONG_MSG_BYTES];
  uint8_t         msg2 [MODES_LONG_MSG_BYTES], *msg;
  uint32_t        j;
  uint8_t        *bestmsg;
  int             bestscore, bestphase;
  const uint16_t *m    = mag->data;
  u_int           mlen = mag->valid_length - mag->overlap;
  uint64_t        sum_scaled_signal_power = 0;

  memset (&mm, '\0', sizeof(mm));
  msg = msg1;

  for (j = 0; j < mlen; j++)
  {
      const uint16_t *preamble = m + j;
      int       high;
      uint32_t  base_signal, base_noise;
      int       msglen, initial_phase, first_phase, last_phase, try_phase;

      /**
       * Look for a message starting at around sample 0 with phase offset 3..7:
       *
       * Ideal sample values for preambles with different phase
       * `Xn` is the first data symbol with phase offset `N`:
       *
       * ```
       * sample#: 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0
       * phase 3: 2/4\0/5\1 0 0 0 0/5\1/3 3\0 0 0 0 0 0 X4
       * phase 4: 1/5\0/4\2 0 0 0 0/4\2 2/4\0 0 0 0 0 0 0 X0
       * phase 5: 0/5\1/3 3\0 0 0 0/3 3\1/5\0 0 0 0 0 0 0 X1
       * phase 6: 0/4\2 2/4\0 0 0 0 2/4\0/5\1 0 0 0 0 0 0 X2
       * phase 7: 0/3 3\1/5\0 0 0 0 1/5\0/4\2 0 0 0 0 0 0 X3
       * ```
       */

      /* Quick check: we must have a rising edge 0->1 and a falling edge 12->13
       */
      if (! (preamble[0] < preamble[1] && preamble[12] > preamble[13]) )
         continue;

      if (preamble[1] > preamble[2] &&                                       /* 1 */
          preamble[2] < preamble[3] && preamble[3] > preamble[4] &&          /* 3 */
          preamble[8] < preamble[9] && preamble[9] > preamble[10] &&         /* 9 */
          preamble[10] < preamble[11])                                       /* 11-12 */
      {
        /* Peaks at 1,3,9,11-12: phase 3
         */
        high = (preamble[1] + preamble[3] + preamble[9] + preamble[11] + preamble[12]) / 4;
        base_signal = preamble[1] + preamble[3] + preamble[9];
        base_noise = preamble[5] + preamble[6] + preamble[7];
      }
      else if (preamble[1] > preamble[2] &&                                  /* 1 */
               preamble[2] < preamble[3] && preamble[3] > preamble[4] &&     /* 3 */
               preamble[8] < preamble[9] && preamble[9] > preamble[10] &&    /* 9 */
               preamble[11] < preamble[12])                                  /* 12 */
      {
        /* Peaks at 1,3,9,12: phase 4
         */
        high = (preamble[1] + preamble[3] + preamble[9] + preamble[12]) / 4;
        base_signal = preamble[1] + preamble[3] + preamble[9] + preamble[12];
        base_noise = preamble[5] + preamble[6] + preamble[7] + preamble[8];
      }
      else if (preamble[1] > preamble[2] &&                                  /* 1 */
               preamble[2] < preamble[3] && preamble[4] > preamble[5] &&     /* 3-4 */
               preamble[8] < preamble[9] && preamble[10] > preamble[11] &&   /* 9-10 */
               preamble[11] < preamble[12])                                  /* 12 */
     {
        /* Peaks at 1,3-4,9-10,12: phase 5
         */
        high = (preamble[1] + preamble[3] + preamble[4] + preamble[9] + preamble[10] + preamble[12]) / 4;
        base_signal = preamble[1] + preamble[12];
        base_noise = preamble[6] + preamble[7];
      }
      else if (preamble[1] > preamble[2] &&                                  /* 1 */
               preamble[3] < preamble[4] && preamble[4] > preamble[5] &&     /* 4 */
               preamble[9] < preamble[10] && preamble[10] > preamble[11] &&  /* 10 */
               preamble[11] < preamble[12])                                  /* 12 */
      {
        /* Peaks at 1,4,10,12: phase 6
         */
        high = (preamble[1] + preamble[4] + preamble[10] + preamble[12]) / 4;
        base_signal = preamble[1] + preamble[4] + preamble[10] + preamble[12];
        base_noise = preamble[5] + preamble[6] + preamble[7] + preamble[8];
      }
      else if (preamble[2] > preamble[3] &&                                  /* 1-2 */
               preamble[3] < preamble[4] && preamble[4] > preamble[5] &&     /* 4 */
               preamble[9] < preamble[10] && preamble[10] > preamble[11] &&  /* 10 */
               preamble[11] < preamble[12])                                  /* 12 */
      {
        /* Peaks at 1-2,4,10,12: phase 7
         */
        high = (preamble[1] + preamble[2] + preamble[4] + preamble[10] + preamble[12]) / 4;
        base_signal = preamble[4] + preamble[10] + preamble[12];
        base_noise  = preamble[6] + preamble[7] + preamble[8];
      }
      else
      {
        /* No suitable peaks
         */
        continue;
      }

      /* Check for enough signal
       */
      if (base_signal * 2 < 3 * base_noise) /* about 3.5dB SNR */
         continue;

      /* Check that the "quiet" bits 6,7,15,16,17 are actually quiet
       */
      if (preamble[5] >= high ||
          preamble[6] >= high ||
          preamble[7] >= high ||
          preamble[8] >= high ||
          preamble[14] >= high ||
          preamble[15] >= high ||
          preamble[16] >= high ||
          preamble[17] >= high ||
          preamble[18] >= high)
      {
        continue;
      }

      if (Modes.phase_enhance)
      {
        first_phase = 4;
        last_phase  = 8;           /* try all phases */
      }
      else
      {
        /* Crosscorrelate against the first few bits to find a likely phase offset
         */
        initial_phase = best_phase (&preamble[19]);
        if (initial_phase < 0)
           continue;       /* nothing satisfactory */

        first_phase = last_phase = initial_phase;  /* try only the phase we think it is */
      }

      Modes.stat.valid_preamble++;
      bestmsg = NULL;
      bestscore = -2;
      bestphase = -1;

      for (try_phase = first_phase; try_phase <= last_phase; try_phase++)
      {
        const uint16_t *phase_ptr;
        int   phase, i, score, bytelen;

        /* Decode all the next 112 bits, regardless of the actual message size.
         * We'll check the actual message type later
         */
        phase_ptr = &m [j + 19] + (try_phase / 5);
        phase     = try_phase % 5;

        bytelen = MODES_LONG_MSG_BYTES;
        for (i = 0; i < bytelen; ++i)
        {
          uint8_t the_byte = 0;

          switch (phase)
          {
            case 0:
                 the_byte = (slice_phase0(phase_ptr) > 0 ? 0x80 : 0) |
                            (slice_phase2(phase_ptr+2) > 0 ? 0x40 : 0) |
                            (slice_phase4(phase_ptr+4) > 0 ? 0x20 : 0) |
                            (slice_phase1(phase_ptr+7) > 0 ? 0x10 : 0) |
                            (slice_phase3(phase_ptr+9) > 0 ? 0x08 : 0) |
                            (slice_phase0(phase_ptr+12) > 0 ? 0x04 : 0) |
                            (slice_phase2(phase_ptr+14) > 0 ? 0x02 : 0) |
                            (slice_phase4(phase_ptr+16) > 0 ? 0x01 : 0);
                 phase = 1;
                 phase_ptr += 19;
                 break;

            case 1:
                 the_byte = (slice_phase1(phase_ptr) > 0 ? 0x80 : 0) |
                            (slice_phase3(phase_ptr+2) > 0 ? 0x40 : 0) |
                            (slice_phase0(phase_ptr+5) > 0 ? 0x20 : 0) |
                            (slice_phase2(phase_ptr+7) > 0 ? 0x10 : 0) |
                            (slice_phase4(phase_ptr+9) > 0 ? 0x08 : 0) |
                            (slice_phase1(phase_ptr+12) > 0 ? 0x04 : 0) |
                            (slice_phase3(phase_ptr+14) > 0 ? 0x02 : 0) |
                            (slice_phase0(phase_ptr+17) > 0 ? 0x01 : 0);
                 phase = 2;
                 phase_ptr += 19;
                 break;

            case 2:
                 the_byte = (slice_phase2(phase_ptr) > 0 ? 0x80 : 0) |
                            (slice_phase4(phase_ptr+2) > 0 ? 0x40 : 0) |
                            (slice_phase1(phase_ptr+5) > 0 ? 0x20 : 0) |
                            (slice_phase3(phase_ptr+7) > 0 ? 0x10 : 0) |
                            (slice_phase0(phase_ptr+10) > 0 ? 0x08 : 0) |
                            (slice_phase2(phase_ptr+12) > 0 ? 0x04 : 0) |
                            (slice_phase4(phase_ptr+14) > 0 ? 0x02 : 0) |
                            (slice_phase1(phase_ptr+17) > 0 ? 0x01 : 0);
                 phase = 3;
                 phase_ptr += 19;
                 break;

            case 3:
                 the_byte = (slice_phase3(phase_ptr) > 0 ? 0x80 : 0) |
                            (slice_phase0(phase_ptr+3) > 0 ? 0x40 : 0) |
                            (slice_phase2(phase_ptr+5) > 0 ? 0x20 : 0) |
                            (slice_phase4(phase_ptr+7) > 0 ? 0x10 : 0) |
                            (slice_phase1(phase_ptr+10) > 0 ? 0x08 : 0) |
                            (slice_phase3(phase_ptr+12) > 0 ? 0x04 : 0) |
                            (slice_phase0(phase_ptr+15) > 0 ? 0x02 : 0) |
                            (slice_phase2(phase_ptr+17) > 0 ? 0x01 : 0);
                 phase = 4;
                 phase_ptr += 19;
                 break;

            case 4:
                 the_byte = (slice_phase4(phase_ptr) > 0 ? 0x80 : 0) |
                            (slice_phase1(phase_ptr+3) > 0 ? 0x40 : 0) |
                            (slice_phase3(phase_ptr+5) > 0 ? 0x20 : 0) |
                            (slice_phase0(phase_ptr+8) > 0 ? 0x10 : 0) |
                            (slice_phase2(phase_ptr+10) > 0 ? 0x08 : 0) |
                            (slice_phase4(phase_ptr+12) > 0 ? 0x04 : 0) |
                            (slice_phase1(phase_ptr+15) > 0 ? 0x02 : 0) |
                            (slice_phase3(phase_ptr+17) > 0 ? 0x01 : 0);
                 phase = 0;
                 phase_ptr += 20;
                 break;
            }

            msg[i] = the_byte;
            if (i == 0)
            {
              switch (msg[0] >> 3)
              {
                case 0:
                case 4:
                case 5:
                case 11:
                     bytelen = MODES_SHORT_MSG_BYTES;
                     break;

                case 16: case 17: case 18:
                case 20: case 21: case 24:
                     break;

                default:
                     bytelen = 1; /* unknown DF, give up immediately */
                     break;
              }
            }
        }

        /* Score the mode S message and see if it's any good
         */
        score = modeS_message_score (msg, i*8);
        if (score > bestscore)
        {
          /* New high score!
           */
          bestmsg = msg;
          bestscore = score;
          bestphase = try_phase;
          Modes.stat.out_of_phase++;

          /* Swap to using the other buffer so we don't clobber our demodulated data
           * (if we find a better result then we'll swap back, but that's OK because
           * we no longer need this copy if we found a better one)
           */
          msg = (msg == msg1) ? msg2 : msg1;
        }
      }

      /* Do we have a candidate?
       */
      if (bestscore < 0)
         continue;    /* nope */

      msglen = modeS_message_len_by_type (bestmsg[0] >> 3);

      /* Set initial mm structure details
       */
      mm.timestamp_msg = mag->sample_timestamp + (j*5) + bestphase;

      /* Compute message receive time as block-start-time + difference in the 12MHz clock
       */
      mm.sys_timestamp_msg = mag->sys_timestamp + receiveclock_ms_elapsed (mag->sample_timestamp, mm.timestamp_msg);

      mm.score = bestscore;
      mm.AC_flags = mm.error_bits = 0;

      Modes.stat.demodulated++;

      /* Decode the received message
       */
      int result = decode_mode_S_message (&mm, bestmsg);
      if (result < 0)
         continue;

      /* Measure signal power
       */
      double   signal_power;
      uint64_t scaled_signal_power = 0;
      int      k, signal_len = (msglen * 12) / 5;

      for (k = 0; k < signal_len; ++k)
      {
        uint32_t _mag = m [j+19+k];
        scaled_signal_power += _mag * _mag;
      }

      signal_power = scaled_signal_power / 65535.0 / 65535.0;
      mm.sig_level = signal_power / signal_len;
      Modes.stat.signal_power_sum   += signal_power;
      Modes.stat.signal_power_count += signal_len;
      sum_scaled_signal_power += scaled_signal_power;

      if (mm.sig_level > Modes.stat.peak_signal_power)
         Modes.stat.peak_signal_power = mm.sig_level;
      if (mm.sig_level > 0.50119)
         Modes.stat.strong_signal_count++;  /* signal power above -3dBFS */

      /*
       * Skip over the message:
       * we actually skip to 8 bits before the end of the message,
       * because we can often decode two messages that *almost* collide,
       * where the preamble of the second message clobbered the last
       * few bits of the first message, but the message bits didn't overlap.
       */
      j += msglen * 12 / 5;

      /* Pass data to the next layer
       */
      modeS_user_message (&mm);

  }  /* for (j = 0; j < mlen; j++) */

  /* Update noise power if measured
   */
  if (Modes.measure_noise)
  {
    double sum_signal_power = sum_scaled_signal_power / 65535.0 / 65535.0;

    Modes.stat.noise_power_sum   += (mag->mean_power - sum_signal_power);
    Modes.stat.noise_power_count += mag->total_length;
  }
}

