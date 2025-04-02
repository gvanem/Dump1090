/**\file    demod-2000.c
 * \ingroup Samplers
 * \brief   Demodulator for 2 MS/s decoding
 */
#include "misc.h"
#include "demod-2000.h"

/**
 * Detect a Mode S messages inside the magnitude buffer pointed by `m`
 * and of size `mlen` bytes. Every detected Mode S message is converted
 * into a stream of bits and passed to the function to display it.
 *
 * In the outer loop to find the preamble and a data-frame:
 *   `mlen == 131310` bits, but `j == [0 .. mlen - (2*120)]`.
 *   Hence `j == [0 .. 131070]`.
 *
 * In the inner loop to extract the bits in a frame:
 *   index `i == [0 .. 2*112]`.
 *
 * \todo Use the pulse_slicer_ppm() function from the RTL-433 project.
 * \ref https://github.com/merbanan/rtl_433/blob/master/src/pulse_slicer.c#L259
 */
uint32_t demodulate_2000 (uint16_t *m, uint32_t mlen)
{
  uint8_t  bits [MODES_LONG_MSG_BITS];
  uint8_t  msg [MODES_LONG_MSG_BITS / 2];
  uint16_t aux [MODES_LONG_MSG_BITS * 2];
  uint32_t j;
  uint32_t frame = 0;
  bool     use_correction = false;
  uint32_t rc = 0;  /**\todo fix this */

  /**
   * The Mode S preamble is made of pulses of 0.5 microseconds
   * at the following time offsets:
   *
   * 0   - 0.5 usec: first pulse.
   * 1.0 - 1.5 usec: second pulse.
   * 3.5 - 4   usec: third pulse.
   * 4.5 - 5   usec: last pulse.
   *
   * Like this  (\ref ../docs/The-1090MHz-riddle.pdf, "1.4.2 Mode S replies"):
   *  ```
   *    < ----------- 8 usec / 16 bits ---------> < ---- data -- ... >
   *    __  __         __  __
   *    | | | |        | | | |
   *    | |_| |________| |_| |__________________  ....
   *
   *    ----|----|----|----|----|----|----|----|
   *    10   10   00   01   01   00   00   00
   * j: 0 1 2 3 4 5 6 7 8 9 10 ...
   * ```
   *
   * If we are sampling at 2 MHz, every sample in our magnitude vector
   * is 0.5 usec. So the preamble will look like this, assuming there is
   * an pulse at offset 0 in the array:
   *
   * ```
   *   0   -----------------
   *   1   -
   *   2   ------------------
   *   3   --
   *   4   -
   *   5   --
   *   6   -
   *   7   ------------------
   *   8   --
   *   9   -------------------
   * ```
   */
  for (j = 0; j < mlen - 2*MODES_FULL_LEN; j++)
  {
    int  low, high, delta, i, errors;
    bool good_message = false;

    if (Modes.exit)
       break;

    if (use_correction)
       goto good_preamble;    /* We already checked it. */

    /* First check of relations between the first 10 samples
     * representing a valid preamble. We don't even investigate further
     * if this simple test is not passed.
     */
    if (!(m[j]   > m[j+1] &&
          m[j+1] < m[j+2] &&
          m[j+2] > m[j+3] &&
          m[j+3] < m[j]   &&
          m[j+4] < m[j]   &&
          m[j+5] < m[j]   &&
          m[j+6] < m[j]   &&
          m[j+7] > m[j+8] &&
          m[j+8] < m[j+9] &&
          m[j+9] > m[j+6]))
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Unexpected ratio among first 10 samples", msg, m, j, frame);

      if (Modes.max_frames > 0 && ++frame > Modes.max_frames)
         return (rc);
      continue;
    }

    /* The samples between the two spikes must be lower than the average
     * of the high spikes level. We don't test bits too near to
     * the high levels as signals can be out of phase so part of the
     * energy can be in the near samples.
     */
    high = (m[j] + m[j+2] + m[j+7] + m[j+9]) / 6;
    if (m[j+4] >= high || m[j+5] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 3 and 6", msg, m, j, frame);

      if (Modes.max_frames > 0 && ++frame > Modes.max_frames)
         return (rc);
      continue;
    }

    /* Similarly samples in the range 11-14 must be low, as it is the
     * space between the preamble and real data. Again we don't test
     * bits too near to high levels, see above.
     */
    if (m[j+11] >= high || m[j+12] >= high || m[j+13] >= high || m[j+14] >= high)
    {
      if ((Modes.debug & DEBUG_NOPREAMBLE) && m[j] > DEBUG_NOPREAMBLE_LEVEL)
         dump_raw_message ("Too high level in samples between 10 and 15", msg, m, j, frame);

      if (Modes.max_frames > 0 && ++frame > Modes.max_frames)
         return (rc);
      continue;
    }

    Modes.stat.valid_preamble++;

good_preamble:

    /* If the previous attempt with this message failed, retry using
     * magnitude correction.
     */
    if (use_correction)
    {
      memcpy (aux, m + j + MODES_PREAMBLE_US * 2, sizeof(aux));
      if (j && detect_out_of_phase(m + j))
      {
        apply_phase_correction (m + j);
        Modes.stat.out_of_phase++;
      }
      /** \todo Apply other kind of corrections. */
    }

    /* Decode all the next 112 bits, regardless of the actual message
     * size. We'll check the actual message type later.
     */
    errors = 0;
    for (i = 0; i < 2 * MODES_LONG_MSG_BITS; i += 2)
    {
      low   = m [j + i + 2*MODES_PREAMBLE_US];
      high  = m [j + i + 2*MODES_PREAMBLE_US + 1];
      delta = low - high;
      if (delta < 0)
         delta = -delta;

      if (i > 0 && delta < 256)
         bits [i/2] = bits [i/2-1];

      else if (low == high)
      {
        /* Checking if two adjacent samples have the same magnitude
         * is an effective way to detect if it's just random noise
         * that was detected as a valid preamble.
         */
        bits [i/2] = 2;    /* error */
        if (i < 2*MODES_SHORT_MSG_BITS)
           errors++;
      }
      else if (low > high)
      {
        bits [i/2] = 1;
      }
      else
      {
        /* (low < high) for exclusion
         */
        bits [i/2] = 0;
      }
    }

    /* Restore the original message if we used magnitude correction.
     */
    if (use_correction)
       memcpy (m + j + 2*MODES_PREAMBLE_US, aux, sizeof(aux));

    /* Pack bits into bytes
     */
    for (i = 0; i < MODES_LONG_MSG_BITS; i += 8)
    {
      msg [i/8] = bits [i]   << 7 |
                  bits [i+1] << 6 |
                  bits [i+2] << 5 |
                  bits [i+3] << 4 |
                  bits [i+4] << 3 |
                  bits [i+5] << 2 |
                  bits [i+6] << 1 |
                  bits [i+7];
    }

    int msg_type = msg[0] >> 3;
    int msg_len  = modeS_message_len_by_type (msg_type) / 8;

    /* Last check, high and low bits are different enough in magnitude
     * to mark this as real message and not just noise?
     */
    delta = 0;
    for (i = 0; i < 8 * 2 * msg_len; i += 2)
    {
      delta += abs (m[j + i + 2 * MODES_PREAMBLE_US] -
                    m[j + i + 2 * MODES_PREAMBLE_US + 1]);
    }
    delta /= 4 * msg_len;

    /* Filter for an average delta of three is small enough to let almost
     * every kind of message to pass, but high enough to filter some
     * random noise.
     */
    if (delta < 10*255)
    {
      use_correction = false;
      continue;
    }

    /* If we reached this point, and error is zero, we are very likely
     * with a Mode S message in our hands, but it may still be broken
     * and CRC may not be correct. This is handled by the next layer.
     */
    if (errors == 0 || (Modes.error_correct_2 && errors <= 2))
    {
      modeS_message mm;
      double        signal_power = 0.0;
      int           signal_len   = mlen;
      uint32_t      k, mag;

      /* Decode the received message and update statistics
       */
      rc += decode_modeS_message (&mm, msg);

      /* measure signal power
       */
      for (k = j; k < j + MODES_FULL_LEN; k++)
      {
        mag = m [k];
        signal_power += mag * mag;
      }
      mm.sig_level = signal_power / (65536.0 * signal_len);

      /* Update statistics.
       */
      if (mm.CRC_ok || use_correction)
      {
        if (errors == 0)
           Modes.stat.demodulated++;
        if (mm.error_bit == -1)
        {
          if (mm.CRC_ok)
               Modes.stat.good_CRC++;
          else Modes.stat.bad_CRC++;
        }
        else
        {
          Modes.stat.bad_CRC++;
          Modes.stat.fixed++;
#if 0
          if (mm.error_bit < MODES_LONG_MSG_BITS)
               Modes.stat.single_bit_fix++;
          else Modes.stat.two_bits_fix++;
#endif
        }
      }

      /* Output debug mode info if needed.
       */
      if (!use_correction)
      {
        if (Modes.debug & DEBUG_DEMOD)
           dump_raw_message ("Demodulated with 0 errors", msg, m, j, frame);

        else if ((Modes.debug & DEBUG_BADCRC) && mm.msg_type == 17 && (!mm.CRC_ok || mm.error_bit != -1))
           dump_raw_message ("Decoded with bad CRC", msg, m, j, frame);

        else if ((Modes.debug & DEBUG_GOODCRC) && mm.CRC_ok && mm.error_bit == -1)
           dump_raw_message ("Decoded with good CRC", msg, m, j, frame);
      }

      /* Skip this message if we are sure it's fine.
       */
      if (mm.CRC_ok)
      {
        j += 2 * (MODES_PREAMBLE_US + (8 * msg_len));
        good_message = true;
        if (use_correction)
           mm.phase_corrected = true;
      }

      /* Pass data to the next layer
       */
      if (mm.CRC_ok)
         modeS_user_message (&mm);
    }
    else
    {
      if ((Modes.debug & DEBUG_DEMODERR) && use_correction)
      {
        LOG_STDOUT ("The following message has %d demod errors:", errors);
        dump_raw_message ("Demodulated with errors", msg, m, j, frame);
      }
    }

    /* Retry with phase correction if possible.
     */
    if (!good_message && !use_correction)
    {
      j--;
      use_correction = true;
    }
    else
    {
      use_correction = false;
    }
  }
  return (rc);
}
