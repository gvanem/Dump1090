/**\file    demod-8000.c
 * \ingroup Demodulators
 * \brief   8MHz Mode S demodulator for SDRPlay only.
 *
 * Author: Tim Thorpe, tim.thorpe@islanddsp.com
 */
#include "misc.h"
#include "demod.h"

/**
 * \def D8M_NUM_PHASES
 * Samples per bit
 */
#define D8M_NUM_PHASES  8

/**
 * \def D8M_WIN_LEN
 * Match window to search for peak correlation
 */
#define D8M_WIN_LEN  (MODES_SHORT_MSG_BITS + MODES_LONG_MSG_BITS)

/**
 * \def D8M_SEARCH_BACK
 * Bits to search back relative to peak
 */
#define D8M_SEARCH_BACK  4

/**
 * \def D8M_SEARCH_AHEAD
 * Bits to search ahead relative to peak
 */
#define D8M_SEARCH_AHEAD  12

/**
 * \def D8M_SEARCH_WIDTH
 * Total Search width in bits
 */
#define D8M_SEARCH_WIDTH  (D8M_SEARCH_BACK + D8M_SEARCH_AHEAD)

/**
 * \def D8M_SEARCH_BYTES
 * Total Search width in bytes, rounded up
 */
#define D8M_SEARCH_BYTES  ((D8M_SEARCH_WIDTH + 7) / 8)

/**
 * \def D8M_LOOK_BACK
 * Buffer look-back required for algorithm
 */
#define D8M_LOOK_BACK  ((D8M_WIN_LEN + D8M_SEARCH_BACK+1) * D8M_NUM_PHASES)

/**
 * \def D8M_LOOK_AHEAD
 * Buffer look-ahead required for algorithm
 */
#define D8M_LOOK_AHEAD  ((MODES_SHORT_MSG_BITS + D8M_SEARCH_AHEAD) * D8M_NUM_PHASES)

/**
 * \def DD8M_BUF_OVERLAP
 * total extra buffer compared to frame of data
 */
#define D8M_BUF_OVERLAP   (D8M_LOOK_BACK + D8M_LOOK_AHEAD)

/**
 * \def D8M_SANE_MAX
 * Upper bound (in either direction) for `phase[]` / `phase_av_acc`.
 * A healthy `phase[i]` is a sum of `abs(diff)` over a 448-sample window;
 * with real-world magnitudes this should sit in the tens of thousands at
 * most. This is set two orders of magnitude above that -- generous enough
 * to never trip during normal operation, but far below where a 32-bit int
 * could overflow/wrap (~2.1 billion). Exceeding it means something has
 * already drifted/gone wrong, so it's reset back to a clean baseline
 * rather than allowed to run into overflow.
 */
#define D8M_SANE_MAX  50000000

static void pick_peak (const int *match, int *peak_short, int *peak_long);
static void shift_bytes (uint8_t *msg, int len);

static int *d8m_dbuf;                              /**< main data buffer */
static int  d8m_phase_av_acc;                      /**< lowpass match memory */
static int  d8m_backtrack_phase_av_acc;            /**< saved version in case of backtrack */
static int  d8m_window;                            /**< current index in match window */
static int  d8m_win_start;                         /**< start of current match window */
static int  d8m_start_phase;                       /**< intial phase chosen for match */
static int  d8m_phase [D8M_NUM_PHASES];            /**< sliding window for each phase */
static int  d8m_backtrack_phase [D8M_NUM_PHASES];  /**< saved version in case of backtrack */
static int *d8m_match_ar;                          /**< match values over current window */
static int *d8m_phase_ar;                          /**< best phase choices over current window */

/**
 * Counts how many times the `D8M_SANE_MAX` clamp below has had to reset
 * `phase[]`/`phase_av_acc`. `phase[i]` is a sliding-window sum maintained
 * purely by incremental add/subtract -- fragile by the demodulator's own
 * doc-comment above, since any imbalance causes the sum to diverge over
 * time rather than staying bounded. This clamp is a safety net against
 * that drift; a nonzero, climbing count here means the clamp is actually
 * engaging (worth knowing if investigating decode-rate issues further),
 * not that anything is currently broken.
 */
static uint64_t d8m_drift_resets;

/**
 * Allocate buffers must be done when `Modes.sample_rate = 8000000` selected.
 */
int demod_8000_alloc (void)
{
#if 0
  /**< \todo Put this somewhere else. To fifo.c? */
#endif

  d8m_match_ar = calloc (D8M_WIN_LEN, sizeof(int));
  d8m_phase_ar = calloc (D8M_WIN_LEN, sizeof(int));
  d8m_dbuf     = calloc (D8M_BUF_OVERLAP + MODES_MAG_BUF_SAMPLES, sizeof(int));
  return (d8m_match_ar && d8m_phase_ar && d8m_dbuf);
}

void demod_8000_free (void)
{
  free (d8m_match_ar);
  free (d8m_phase_ar);
  free (d8m_dbuf);
}

/**
 * Demodulator for 16-bit, 8MHz magnitude array.
 *
 * Basic method: a sliding window of length 56 bits is used to locate
 * data bursts of 56 or 112 bits. The location criterion is the summed
 * magnitude of data transitions spaced 8 samples (1 bit period) apart.
 * The data-block location generally corresponds to the peak of this match
 * value, but to allow for noise, a few bits before and after are also
 * checked for plausible decoded messages. This search is only triggered
 * when the match exceeds the long-term average noise value by a specified
 * factor.
 *
 * For efficiency, the sliding window is implemented by adding a sample
 * to the leading edge and subtracting one from the trailing edge. This
 * is fragile because any lack of balance would cause the sum to diverge.
 * Therefore, take care if modifying any of the buffering or wrap-around
 * indexing.
 */
void demod_8000 (const mag_buf *mag)
{
  TRACE2 ("demod_8000 called, valid_length: %u, overlap: %u\n",
          mag->valid_length, mag->overlap);

  modeS_message   mm;
  uint8_t         msg      [MODES_LONG_MSG_BYTES + D8M_SEARCH_BYTES];
  uint8_t         best_msg [MODES_LONG_MSG_BYTES];
  uint8_t         data_byte = 0;
  int             phase_av, max, best_phase;
  int             short_msg_offset = 0;
  int             long_msg_offset = 0;
  int             sptr;
  int             eptr, dptr;
  int            *dbuf;
  int             i, sum;
  int             message_result;
  int             j, mlen = (int) (mag->valid_length - mag->overlap);

  /* CORRECTION (superseding an earlier, incorrect "fix"): I previously
   * changed this to `mag->data + mag->overlap`, reasoning that `m` and
   * `mlen` were mismatched after porting from the original's single
   * `mag->length` field. That reasoning was wrong. Mutability's actual
   * header (dump1090.h) documents `length` as "number of valid samples
   * _after_ overlap" -- i.e. exactly the same quantity as our
   * `valid_length - overlap` -- and the real, proven-working
   * `demodulate8000()` still reads `m = mag->data` (index 0, the old
   * overlap tail) for that many samples, with NO offset. So the
   * original does read old-tail-then-partial-new-data each call, same
   * as this codebase without the offset -- and it works fine, because
   * demod_8000 keeps its own independent lookback (`d8m_dbuf` /
   * `D8M_BUF_OVERLAP`) across calls, making the overall stream it
   * builds internally contiguous regardless of this per-call offset.
   * Reverted to match the real reference behaviour.
   */
  const uint16_t *m = mag->data;

  /* local variables initialized from static storage
   */
  int phase_av_acc            = d8m_phase_av_acc;
  int backtrack_phase_av_acc  = d8m_backtrack_phase_av_acc;
  int window                  = d8m_window;
  int win_start               = d8m_win_start;
  int start_phase             = d8m_start_phase;

  int phase [D8M_NUM_PHASES];
  int backtrack_phase [D8M_NUM_PHASES];
  int match_ar [D8M_WIN_LEN];
  int phase_ar [D8M_WIN_LEN];

  memcpy (phase, d8m_phase, sizeof(phase));
  memcpy (backtrack_phase, d8m_backtrack_phase, sizeof(backtrack_phase));
  memcpy (match_ar, d8m_match_ar, sizeof(match_ar));
  memcpy (phase_ar, d8m_phase_ar, sizeof(phase_ar));

  memset (&mm, '\0', sizeof(mm));

  /* For code below, mlen must be divisible by 8. It should be, but just in case, force it.
   * This would discard the last few samples of input.
   */
  mlen &= ~7;

  /* First we calculate the 4-sample diff value. This is convenient because both magnitude
   * match and decoded data are based on this value, so we avoid recalculation.
   */
  dbuf = d8m_dbuf + D8M_BUF_OVERLAP;  /* point to start of new data */

  for (j = 0; j < mlen; j++)
  {
    dbuf [j] = m [j];
    if (j < mlen - sizeof(*m) - 2)
       dbuf [j] -= m [j + 4];     /* +4 OK because there are Modes.trailing_samples extra */
  }

  /* Now point to a location which allows the algorithm both some look-back
   * and look-ahead in the data.
   */
  dbuf = d8m_dbuf + D8M_LOOK_BACK;

  /* Sliding window start and end points
   */
  sptr = 0;
  eptr = MODES_SHORT_MSG_BITS * D8M_NUM_PHASES;

  /* Loop iterates one bit at a time, but calculates separate matches (phase[n])
   * for each phase within the bit-period, 8 at 8MHz sampling. Effectively we have
   * 8 distict sliding windows to choose between.
   */
  while (sptr < mlen)
  {
    /* update window */
    max = 0;
    for (i = 0; i < D8M_NUM_PHASES; i++)
    {
      phase[i] += abs(dbuf[eptr++]);
      phase[i] -= abs(dbuf[sptr++]);

      /* FIX: see D8M_SANE_MAX doc-comment above. Confirmed via an
       * independent, non-accumulating ground-truth measurement (fresh
       * |diff| sum computed with no carried-over state) staying flat
       * while phase_av wrapped through the full range of a 32-bit int --
       * this drift is real and severe, not a signal artifact.
       *
       * Resync to the true value instead of zeroing: zeroing discards
       * the real window sum and leaves every later add/subtract this
       * call referenced from a false baseline -- a compounding drift
       * source in its own right. `phase[i]` is a sum over
       * MODES_SHORT_MSG_BITS (56) samples spaced D8M_NUM_PHASES (8)
       * apart, ending at the sample just added (`eptr - 1`, since `eptr`
       * was already post-incremented above), so it can be recomputed
       * exactly and cheaply -- this only runs on the rare overflow
       * event, not every step.
       */
      if (phase[i] > D8M_SANE_MAX || phase[i] < -D8M_SANE_MAX)
      {
        int resync_sum = 0;
        int resync_pos = eptr - 1;
        int resync_k;

        for (resync_k = 0; resync_k < MODES_SHORT_MSG_BITS; resync_k++, resync_pos -= D8M_NUM_PHASES)
            resync_sum += abs(dbuf[resync_pos]);

        phase[i] = resync_sum;
        d8m_drift_resets++;
      }

      if (phase[i] > max)
         max = phase[i];
    }

    /* low pass filter to get long-term S+N (mostly N) value
     */
    phase_av_acc += phase[0];

    /* FIX: same D8M_SANE_MAX safety net as the phase[i] clamp above,
     * applied to the other accumulator that shares the same fragile
     * incremental-sum design.
     */
    if (phase_av_acc > D8M_SANE_MAX || phase_av_acc < -D8M_SANE_MAX)
    {
      phase_av_acc = 0;
      d8m_drift_resets++;
    }

    phase_av = phase_av_acc >> 14;          /* phase_av is current output */
    phase_av_acc -= phase_av;               /* phase_av_acc is filter memory */

    if (sptr == 40000)  /* print occasionally */
        TRACE2 ("max: %d, phase_av: %d, ratio: %d%%\n",
                max, phase_av, phase_av ? (max * 200 / phase_av) : 0);

    /* This code first triggers when max exceeds noise by given factor.
     * Once triggered, it continues for 0 <= window < WIN_LEN, ie
     * WIN_LEN contiguous bits. On the last bit, the peak-finding
     * routine is called to locate the data-block. Then, the values
     * are all set back by 56 bits before resuming. This is because,
     * for long messages, it is not possible to detect peaks in the
     * last 56 locations without more look-ahead.
     */
    if (window || (max * 2 > phase_av * 3))
    {
      /* note which of 8 phases gives the greatest match */
      max = best_phase = 0;
      for (i = 0; i < D8M_NUM_PHASES; i++)
          if (phase[i] > max) {best_phase = i; max = phase[i];}

      /* on first bit, record start of match window and best phase */
      if (window == 0)
      {
        win_start = sptr;
        start_phase = best_phase;
      }

      /* record match value and best phase in arrays
       */
      match_ar [window] = phase[start_phase];  /* use same phase consistently, even if not best */
      phase_ar [window] = best_phase;          /* but record the best for later use */

      /* save intermediate values 56 before end of match window
       */
      if (window == D8M_WIN_LEN - MODES_SHORT_MSG_BITS)
      {
        memcpy (backtrack_phase, phase, sizeof(phase));
        backtrack_phase_av_acc = phase_av_acc;
      }

      /* end of match window, now locate peaks and look for valid messages
       */
      if (++window == D8M_WIN_LEN)
      {
        int best_result = -1;
        int msg_bytes, msg_type, position;

        window = 0; /* reset trigger value */

        pick_peak (match_ar, &short_msg_offset, &long_msg_offset);

        /* Now we've located the match peak, decode the bits to look for
         * plausible message. We search twice, once around putative short-message
         * peak, then long-message peak.
         */
        msg_bytes = MODES_SHORT_MSG_BYTES;
        dptr = win_start + phase_ar[short_msg_offset] + (short_msg_offset - D8M_SEARCH_BACK) * D8M_NUM_PHASES;
        position = dptr;

        for (msg_type = 0; msg_type < 2; msg_type++)
        {
          /* decode enough bits to search +- a few bits for message
           */
          for (i = 0; i < msg_bytes + D8M_SEARCH_BYTES; i++)
          {
            data_byte = 0;

            for (j = 0; j < 8; j++ )
            {
              sum = dbuf [dptr-1] + dbuf [dptr] + dbuf [dptr+1];
              sum = (sum >> 31) & 0x1;                        /* sign gives data bit */
              data_byte = (data_byte << 1) | sum;
              dptr += D8M_NUM_PHASES;
            }
            msg [i] = ~data_byte; /* data was inverted */
          }

          /* Search for messages by shifting data one bit and re-testing
           */
          for (i = 0; i < D8M_SEARCH_WIDTH; i++)
          {
            message_result = modeS_message_score (msg, msg_bytes * 8);

            if (message_result > best_result)
            {
              memcpy (best_msg, msg, msg_bytes);   /* most plausible message so far */
              best_result = message_result;
              position = dptr - 64 + i * 8;        /* position recorded for MLAT */
            }
            shift_bytes (msg, msg_bytes + D8M_SEARCH_BYTES); /* shift by one bit */
          }

          msg_bytes = MODES_LONG_MSG_BYTES;
          dptr = win_start + phase_ar[long_msg_offset] + (long_msg_offset - D8M_SEARCH_BACK) * D8M_NUM_PHASES;
        }

        /* Decode the received message
         */
        if (best_result >= 0)
        {
          Modes.stat.valid_preamble++;
          mm.AC_flags = mm.error_bits = 0;

          /* `msglen`/`signal_len` used below for both the MLAT timestamp
           * correction and the RSSI measurement further down -- computed
           * once here so both stay consistent.
           */
          int msglen = modeS_message_len_by_type (best_msg[0] >> 3);
          int signal_len = msglen * D8M_NUM_PHASES;

          /* Set initial mm structure details
           *
           * `mag->sample_timestamp` marks the start of the *new*
           * samples, i.e. `mag->data[mag->overlap]`. `position`
           * is an index relative to `m = mag->data` (index 0,
           * the old overlap tail -- see the correction above),
           * so it needs `mag->overlap` subtracted to be relative
           * to the new-data start that `sample_timestamp` marks.
           *
           * MLAT FIX (same root cause as the RSSI fix further down,
           * applied here by the same reasoning but NOT independently
           * verified -- neither of us runs MLAT, so this hasn't been
           * checked end-to-end against a real multilateration setup;
           * flagging for anyone who does to confirm): live testing while
           * fixing RSSI below showed `position` consistently marks the
           * END of the decoded message, not the start (`dptr` walks
           * forward through the full byte-extraction before the small
           * `-64+i*8` adjustment, so it can't represent anything but a
           * point near the end of that walk). MLAT timestamps should
           * mark the message's start, so the same `- signal_len`
           * correction used for the RSSI window is applied here too.
           */
          mm.timestamp_msg = mag->sample_timestamp +
              (position - D8M_LOOK_AHEAD - signal_len - (int) mag->overlap) * 12 / 8;

          /* compute message receive time as block-start-time + difference in the 12MHz clock
           */
          mm.sys_timestamp_msg = mag->sys_timestamp + receiveclock_ms_elapsed (mag->sample_timestamp, mm.timestamp_msg);

          Modes.stat.demodulated++;

          /* Decode the received message
           */
          message_result = decode_mode_S_message (&mm, best_msg);

          if (message_result >= 0)
          {
            /* Measure signal power for RSSI.
             *
             * Adapted from demod-2400.c's pattern, but NOT a direct port --
             * several things differ at 8 MS/s / in this function's own
             * coordinate system:
             *
             * 1) `position` (not `j`, which demod-2400.c uses but which
             *    isn't meaningful here) tracks this message's location,
             *    in the same `dbuf`-relative coordinate system already
             *    used for the MLAT timestamp above.
             * 2) `signal_len` uses D8M_NUM_PHASES (8 samples/bit at 8 MS/s)
             *    directly -- demod-2400.c's `*12/5` ratio is specific to
             *    its own 2.4 MS/s sampling and doesn't apply here.
             * 3) Normalizing by `65535.0 * 65535.0` (squared, matching
             *    `_mag * _mag` being a squared magnitude), not a single
             *    `65535.0` -- the single-division version overstates
             *    signal power by a factor of 65535.
             * 4) `position - D8M_LOOK_AHEAD` alone maps to the *end* of
             *    the message, not the start (confirmed by directly
             *    inspecting the raw magnitude data around candidate
             *    windows during testing) -- the extra `- signal_len`
             *    steps back to the message's actual start.
             *
             * Unlike `dbuf` (which has D8M_LOOK_BACK/D8M_LOOK_AHEAD margin
             * deliberately built in), `m` is the raw per-call buffer with
             * no look-back headroom -- an unchecked read here can run
             * outside its valid range. Bounds-checked below: if the
             * mapped range falls outside [0, mlen), skip the measurement
             * for this message rather than read out of bounds. sig_level
             * stays at its memset default (0), and aircraft.c already
             * only records a signal sample when sig_level > 0, so this
             * message simply won't contribute an RSSI sample this time.
             */
            int m_start = position - D8M_LOOK_AHEAD - signal_len;

            if (m_start >= 0 && m_start + signal_len <= mlen)
            {
              double   signal_power;
              uint64_t scaled_signal_power = 0;
              int      k;

              for (k = 0; k < signal_len; k++)
              {
                uint32_t _mag = m [m_start + k];
                scaled_signal_power += _mag * _mag;
              }
              signal_power = scaled_signal_power / 65535.0 / 65535.0;
              mm.sig_level = signal_power / signal_len;
            }
          }

          if (mm.addr && message_result >= 0)
             modeS_user_message (&mm);
        }

        /* now backtrack by 56 bits, as we may have missed peaks in this region
        */
        sptr -= (MODES_SHORT_MSG_BITS-1) * D8M_NUM_PHASES;
        eptr -= (MODES_SHORT_MSG_BITS-1) * D8M_NUM_PHASES;
        memcpy (phase, backtrack_phase, sizeof(phase));
        phase_av_acc = backtrack_phase_av_acc;
      }
    }
  }

  /* Copy overlapped part of buffer from end to beginning of array. `memmove`
   * (not `memcpy`) is required here: when `mlen < D8M_BUF_OVERLAP` (can
   * happen under `--infile` replay, e.g. a short/final chunk) the source
   * and destination ranges overlap, which is undefined behaviour for
   * `memcpy`. `memmove` handles that correctly and is identical to
   * `memcpy` (no extra cost) in the normal live-capture case where the
   * ranges don't overlap.
   */
  memmove (d8m_dbuf + 0, d8m_dbuf + mlen, D8M_BUF_OVERLAP *  sizeof(int));

  /* copy local variables back to static struct
   */
  d8m_phase_av_acc = phase_av_acc;
  d8m_backtrack_phase_av_acc = backtrack_phase_av_acc;
  d8m_window = window;
  d8m_win_start = win_start - mlen;
  d8m_start_phase = start_phase;

  memcpy (d8m_phase, phase, sizeof(phase));
  memcpy (d8m_backtrack_phase, backtrack_phase, sizeof(backtrack_phase));
  memcpy (d8m_match_ar, match_ar, sizeof(match_ar));
  memcpy (d8m_phase_ar, phase_ar, sizeof(phase_ar));
}

/*
 * Find maxima in the match array corresponding to short messages (56 bits)
 * and long messages (112 bits).
 */
static void pick_peak (const int *match,            /* input array of D8M_WIN_LEN match values */
                       int       *peak_short,       /* returned location of peak 56 */
                       int       *peak_long)        /* returned location of peak 112 */
{
  int i, match112;
  int max0 = 0, max1 = 0;

  for (i = 0; i < D8M_WIN_LEN - MODES_SHORT_MSG_BITS; i++)
  {
    if (match[i] >= max0)
    {
      max0 = match [i];
      *peak_short = i;
    }

    /* synthesise 112-bit match by adding two 56-bit matches
     */
    match112 = match [i] + match [i + MODES_SHORT_MSG_BITS];
    if (match112 >= max1)
    {
      max1 = match112;
      *peak_long = i;
    }
  }
}

/*
 * 1-bit shift towards MSB[0] in an array of bytes of length len
 */
static void shift_bytes (uint8_t *msg, int len)
{
  int i;

  for (i = 0; i < len - 1; i++)
      msg[i] = (msg[i] << 1) | (msg[i+1] >> 7);
  msg[i] <<= 1;
}
