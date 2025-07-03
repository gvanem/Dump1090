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
  modeS_message   mm;
  uint8_t         msg      [MODES_LONG_MSG_BYTES + D8M_SEARCH_BYTES];
  uint8_t         best_msg [MODES_LONG_MSG_BYTES];
  uint8_t         data_byte = 0;
  int             phase_av, max, best_phase;
  int             short_msg_offset = 0;
  int             long_msg_offset = 0;
  u_int           sptr;
  int             eptr, dptr;
  int            *dbuf;
  int             i, sum;
  int             message_result;
  u_int           j, mlen = mag->valid_length - mag->overlap;
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
    dbuf [j] -= m [j+4];        /* +4 OK because there are Modes.trailing_samples extra */
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
      if (phase[i] > max)
         max = phase[i];
    }

    /* low pass filter to get long-term S+N (mostly N) value
     */
    phase_av_acc += phase[0];
    phase_av = phase_av_acc >> 14;          /* phase_av is current output */
    phase_av_acc -= phase_av;               /* phase_av_acc is filter memory */

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

          /* Set initial mm structure details
           */
          mm.timestamp_msg = mag->sample_timestamp + (position - D8M_LOOK_AHEAD) * 12 / 8;

          /* compute message receive time as block-start-time + difference in the 12MHz clock
           */
          mm.sys_timestamp_msg = mag->sys_timestamp + receiveclock_ms_elapsed (mag->sample_timestamp, mm.timestamp_msg);

          Modes.stat.demodulated++;

          /* Decode the received message
           */
          message_result = decode_mode_S_message (&mm, best_msg);

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

  /* Copy overlapped part of buffer from end to beginning of array
   */
  memcpy (d8m_dbuf + 0, d8m_dbuf + mlen, D8M_BUF_OVERLAP *  sizeof(int));

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
