/*
 * Part of dump1090, a Mode S message decoder for RTLSDR / SDRPlay.
 *
 * Copyright (c) 2014,2015 Oliver Jowett <oliver@mutability.co.uk>
 */

/**\file    demod-2000.c
 * \ingroup Demodulators
 * \brief   2 MHz Mode A/C detection and decoding.
 */
#include "misc.h"
#include "crc.h"
#include "demod.h"

/**
 * This table is used to build the Mode A/C variable called ModeABits.
 * Each bit period is inspected, and if it's value exceeds the threshold limit,
 * then the value in this table is or-ed into ModeABits.
 *
 * At the end of message processing, ModeABits will be the decoded ModeA value.
 *
 * We can also flag noise in bits that should be zeros - the xx bits. Noise in
 * these bits cause bits (31-16) in ModeABits to be set. Then at the end of message
 * processing we can test for errors by looking at these bits.
 */
static uint32_t ModeABitTable [24] = {
                0x00000000,   // F1 = 1
                0x00000010,   // C1
                0x00001000,   // A1
                0x00000020,   // C2
                0x00002000,   // A2
                0x00000040,   // C4
                0x00004000,   // A4
                0x40000000,   // xx = 0  Set bit 30 if we see this high
                0x00000100,   // B1
                0x00000001,   // D1
                0x00000200,   // B2
                0x00000002,   // D2
                0x00000400,   // B4
                0x00000004,   // D4
                0x00000000,   // F2 = 1
                0x08000000,   // xx = 0  Set bit 27 if we see this high
                0x04000000,   // xx = 0  Set bit 26 if we see this high
                0x00000080,   // SPI
                0x02000000,   // xx = 0  Set bit 25 if we see this high
                0x01000000,   // xx = 0  Set bit 24 if we see this high
                0x00800000,   // xx = 0  Set bit 23 if we see this high
                0x00400000,   // xx = 0  Set bit 22 if we see this high
                0x00200000,   // xx = 0  Set bit 21 if we see this high
                0x00100000,   // xx = 0  Set bit 20 if we see this high
              };

/**
 * This table is used to produce an error variable called ModeAErrs. Each
 * inter-bit period is inspected, and if it's value falls outside of the
 * expected range, then the value in this table is or-ed into ModeAErrs.
 *
 * At the end of message processing, ModeAErrs will indicate if we saw
 * any inter-bit anomalies, and the bits that are set will show which
 * bits had them.
 */
static uint32_t ModeAMidTable [24] = {
                0x80000000,   // F1 = 1  Set bit 31 if we see F1_C1  error
                0x00000010,   // C1      Set bit  4 if we see C1_A1  error
                0x00001000,   // A1      Set bit 12 if we see A1_C2  error
                0x00000020,   // C2      Set bit  5 if we see C2_A2  error
                0x00002000,   // A2      Set bit 13 if we see A2_C4  error
                0x00000040,   // C4      Set bit  6 if we see C3_A4  error
                0x00004000,   // A4      Set bit 14 if we see A4_xx  error
                0x40000000,   // xx = 0  Set bit 30 if we see xx_B1  error
                0x00000100,   // B1      Set bit  8 if we see B1_D1  error
                0x00000001,   // D1      Set bit  0 if we see D1_B2  error
                0x00000200,   // B2      Set bit  9 if we see B2_D2  error
                0x00000002,   // D2      Set bit  1 if we see D2_B4  error
                0x00000400,   // B4      Set bit 10 if we see B4_D4  error
                0x00000004,   // D4      Set bit  2 if we see D4_F2  error
                0x20000000,   // F2 = 1  Set bit 29 if we see F2_xx  error
                0x08000000,   // xx = 0  Set bit 27 if we see xx_xx  error
                0x04000000,   // xx = 0  Set bit 26 if we see xx_SPI error
                0x00000080,   // SPI     Set bit 15 if we see SPI_xx error
                0x02000000,   // xx = 0  Set bit 25 if we see xx_xx  error
                0x01000000,   // xx = 0  Set bit 24 if we see xx_xx  error
                0x00800000,   // xx = 0  Set bit 23 if we see xx_xx  error
                0x00400000,   // xx = 0  Set bit 22 if we see xx_xx  error
                0x00200000,   // xx = 0  Set bit 21 if we see xx_xx  error
                0x00100000,   // xx = 0  Set bit 20 if we see xx_xx  error
              };

/**
 * The "off air" format is:
 * ```
 *  _F1_C1_A1_C2_A2_C4_A4_xx_B1_D1_B2_D2_B4_D4_F2_xx_xx_SPI_
 * ```
 *
 * Bit spacing is 1.45uS, with 0.45uS high, and 1.00us low. This is a problem
 * because we are sampling at 2MHz (500nS) so we are below Nyquist.
 *
 * The bit spacings are:
 * ```
 *  F1 :  0.00,
 *        1.45,  2.90,  4.35,  5.80,  7.25,  8.70,
 *  X  : 10.15,
 *     : 11.60, 13.05, 14.50, 15.95, 17.40, 18.85,
 *  F2 : 20.30,
 *  X  : 21.75, 23.20, 24.65
 * ```
 *
 * This equates to the following sample point centers at 2MHz:
 * ```
 * [ 0.0],
 * [ 2.9], [ 5.8], [ 8.7], [11.6], [14.5], [17.4],
 * [20.3],
 * [23.2], [26.1], [29.0], [31.9], [34.8], [37.7]
 * [40.6]
 * [43.5], [46.4], [49.3]
 * ```
 *
 * We know that this is a supposed to be a binary stream, so the signal
 * should either be a 1 or a 0. Therefore, any energy above the noise level
 * in two adjacent samples must be from the same pulse, so we can simply
 * add the values together..
 */
static int detect_mode_A (uint16_t *m, modeS_message *mm)
{
  int j, lastBitWasOne;
  int ModeABits = 0;
  int ModeAErrs = 0;
  int byte, bit;
  int thisSample, lastBit, lastSpace = 0;
  int m0, m1, m2, m3, mPhase;
  int n0, n1, n2 ,n3;
  int F1_sig, F1_noise;
  int F2_sig, F2_noise;
  int fSig, fNoise, fLevel, fLoLo;

  /**
   * `m[0]` contains the energy from    0 ->  499 nS <br>
   * `m[1]` contains the energy from  500 ->  999 nS <br>
   * `m[2]` contains the energy from 1000 -> 1499 nS <br>
   * `m[3]` contains the energy from 1500 -> 1999 nS <br>
   *
   * We are looking for a Frame bit (F1) whose width is 450nS, followed by
   * 1000nS of silence.
   *
   * The width of the frame bit is 450nS, which is 90% of our sample rate.
   * Therefore, in an ideal world, all the energy for the frame bit will be
   * in a single sample, preceeded by (at least) one zero, and followed by
   * two zeros, Best case we can look for:
   * ```
   *  0 - 1 - 0 - 0
   * ```
   *
   * However, our samples are not phase aligned, so some of the energy from
   * each bit could be spread over two consecutive samples. Worst case is
   * that we sample half in one bit, and half in the next. In that case,
   * we're looking for:
   * ```
   * 0 - 0.5 - 0.5 - 0.
   * ```
   */
  m0 = m[0];
  m1 = m[1];

  if (m0 >= m1)   /* m1 *must* be bigger than m0 for this to be F1 */
     return (0);

  m2 = m[2];
  m3 = m[3];

  /* if (m2 <= m0), then assume the sample bob on (Phase == 0), so don't look at m3
   */
  if (m2 <= m0 || m2 < m3)
  {
    m3 = m2;
    m2 = m0;
  }

  if (m3 >= m1 ||   /* m1 must be bigger than m3 */
      m0 >  m2 ||   /* m2 can be equal to m0 if ( 0,1,0,0 ) */
      m3 >  m2)     /* m2 can be equal to m3 if ( 0,1,0,0 ) */
    return (0);

  /* m0 = noise
   * m1 = noise + (signal *    X))
   * m2 = noise + (signal * (1-X))
   * m3 = noise
   *
   * Hence, assuming all 4 samples have similar amounts of noise in them:
   *   signal = (m1 + m2) - ((m0 + m3) * 2)
   *   noise  = (m0 + m3) / 2
   */
  F1_sig   = (m1 + m2) - ((m0 + m3) << 1);
  F1_noise = (m0 + m3) >> 1;

  if ((F1_sig < MODEAC_MSG_SQUELCH_LEVEL) ||  /* minimum required  F1 signal amplitude */
      (F1_sig < (F1_noise << 2)))             /* minimum allowable Sig/Noise ratio 4:1 */
   return (0);

  /* If we get here then we have a potential F1, so look for an equally valid F2 20.3uS later
   *
   * Our F1 is centered somewhere between samples m[1] and m[2]. We can guestimate where F2 is
   * by comparing the ratio of m1 and m2, and adding on 20.3 uS (40.6 samples)
   */
  mPhase = ((m2 * 20) / (m1 + m2));
  byte   = (mPhase + 812) / 20;
  n0     = m[byte++]; n1 = m[byte++];

  if (n0 >= n1)   /* n1 *must* be bigger than n0 for this to be F2 */
     return (0);

  n2 = m[byte++];

  /* if the sample bob on (Phase == 0), don't look at n3
   */
  if ((mPhase + 812) % 20)
    n3 = m[byte++];
  else
  {
    n3 = n2;
    n2 = n0;
  }

  if ((n3 >= n1) ||  /* n1 must be bigger than n3 */
      (n0 >  n2) ||  /* n2 can be equal to n0 ( 0,1,0,0 ) */
      (n3 >  n2))    /* n2 can be equal to n3 ( 0,1,0,0 ) */
    return (0);

  F2_sig   = (n1 + n2) - ((n0 + n3) << 1);
  F2_noise = (n0 + n3) >> 1;

  if (F2_sig < MODEAC_MSG_SQUELCH_LEVEL || /* minimum required  F2 signal amplitude */
      F2_sig < (F2_noise << 2))            /* maximum allowable Sig/Noise ratio 4:1 */
    return (0);

  fSig          = (F1_sig   + F2_sig)   >> 1;
  fNoise        = (F1_noise + F2_noise) >> 1;
  fLoLo         = fNoise    + (fSig >> 2);       /* 1/2 */
  fLevel        = fNoise    + (fSig >> 1);
  lastBitWasOne = 1;
  lastBit       = F1_sig;

  /* Now step by a half ModeA bit, 0.725nS, which is 1.45 samples, which is 29/20
   * No need to do bit 0 because we've already selected it as a valid F1
   * Do several bits past the SPI to increase error rejection
   */
  for (j = 1, mPhase += 29; j < 48; mPhase += 29, j ++)
  {
    byte = 1 + (mPhase / 20);

    thisSample = m[byte] - fNoise;
    if (mPhase % 20)                        /* If the bit is split over two samples.. */
       thisSample += (m[byte+1] - fNoise);  /*   add in the second sample's energy */

    /* If we're calculating a space value */
    if (j & 1)
       lastSpace = thisSample;

    else
    {
      /* We're calculating a new bit value */
      bit = j >> 1;
      if (thisSample >= fLevel)
      {
        /* We're calculating a new bit value, and its a one */
        ModeABits |= ModeABitTable[bit--];  /* or in the correct bit */

        if (lastBitWasOne)
        {
          /* This bit is one, last bit was one, so check the last space is somewhere less than one
           */
          if (lastSpace >= (thisSample >> 1) || lastSpace >= lastBit)
             ModeAErrs |= ModeAMidTable[bit];
        }
        else
        {
          /* This bit,is one, last bit was zero, so check the last space is somewhere less than one
           */
          if (lastSpace >= (thisSample >> 1))
             ModeAErrs |= ModeAMidTable[bit];
        }
        lastBitWasOne = 1;
      }

      else
      {
        /* We're calculating a new bit value, and its a zero
         */
        if (lastBitWasOne)
        {
          /* This bit is zero, last bit was one, so check the last space is somewhere in between
           */
          if (lastSpace >= lastBit)
             ModeAErrs |= ModeAMidTable[bit];
        }
        else
        {
          /* This bit,is zero, last bit was zero, so check the last space is zero too
           */
          if (lastSpace >= fLoLo)
             ModeAErrs |= ModeAMidTable[bit];
        }
        lastBitWasOne = 0;
      }
      lastBit = (thisSample >> 1);
    }
  }

  /* Output format is : 00:A4:A2:A1:00:B4:B2:B1:00:C4:C2:C1:00:D4:D2:D1
   */
  if ((ModeABits < 3) || (ModeABits & 0xFFFF8808) || (ModeAErrs) )
     return (ModeABits = 0);

  mm->sig_level = (fSig + fNoise) * (fSig + fNoise) / MAX_POWER;

  return (ModeABits);
}

/**
 *============================= Debugging =================================
 *
 * Helper function for dump_mag_vector().
 * It prints a single bar used to display raw signals.
 *
 * Since every magnitude sample is between 0-255, the function uses
 * up to 63 characters for every bar. Every character represents
 * a length of 4, 3, 2, 1, specifically:
 * ```
 *   "O" = 4
 *   "o" = 3
 *   "-" = 2
 *   "." = 1
 * ```
 */
static void dump_mag_bar (int index, uint16_t magnitude)
{
  const char *set = " .-o";
  char        buf [256];
  int         div = magnitude / 256 / 4;
  int         rem = magnitude / 256 % 4;

  memset (buf, 'O', div);
  buf [div] = set [rem];
  buf [div+1] = '\0';

  if (index >= 0)
       printf ("[%.3d] |%-66s 0x%04X\n", index, buf, magnitude);
  else printf ("[%.2d] |%-66s 0x%04X\n", index, buf, magnitude);
}

/**
 * Display an ASCII-art alike graphical representation of the undecoded
 * message as a magnitude signal.
 *
 * The message starts at the specified offset in the "m" buffer.
 * The function will display enough data to cover a short 56 bit message.
 *
 * If possible a few samples before the start of the messsage are included
 * for context.
 */
static void dump_mag_vector (const uint16_t *m, uint32_t offset)
{
  uint32_t padding = 5;   /* Show a few samples before the actual start */
  uint32_t start = (offset < padding) ? 0 : offset-padding;
  uint32_t end = offset + (MODES_PREAMBLE_SAMPLES)+(MODES_SHORT_MSG_SAMPLES) - 1;
  uint32_t j;

  for (j = start; j <= end; j++)
      dump_mag_bar (j-offset, m[j]);
}

/*
 * Produce a raw representation of the message as a Javascript file
 * loadable by debug.html.
 */
static bool dump_raw_message_JS (const char      *descr, const uint8_t *msg,
                                 const uint16_t  *m,     uint32_t offset,
                                 const errorinfo *ei)
{
  int   j, padding = 5; /* Show a few samples before the actual start */
  int   start = offset - padding;
  int   end = offset + MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 1;
  FILE *fp = fopen("frames.js", "a");

  if (!fp)
  {
    LOG_STDERR ("Error opening frames.js: %s\n", strerror(errno));
    return (false);
  }

  fprintf (fp,"frames.push({\"descr\": \"%s\", \"mag\": [", descr);
  for (j = start; j <= end; j++)
  {
    fprintf(fp,"%d", j < 0 ? 0 : m[j]);
    if (j != end)
       fprintf (fp, ",");
  }

  fprintf (fp, "], ");
  for (j = 0; j < MODES_MAX_BITERRORS; j++)
      fprintf (fp, "\"fix%d\": %d, ", j, ei->bit[j]);

  fprintf (fp, "\"bits\": %d, \"hex\": \"", modeS_message_len_by_type(msg[0] >> 3));
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
      fprintf (fp, "\\x%02x", msg[j]);
  fprintf (fp, "\"});\n");
  fclose (fp);
  return (true);
}

/*
 * This is a wrapper for dump_mag_vector() that also show the message
 * in hex format with an additional description.
 *
 * descr  is the additional message to show to describe the dump.
 * msg    points to the decoded message
 * m      is the original magnitude vector
 * offset is the offset where the message starts
 *
 * The function also produces the Javascript file used by debug.html to
 * display packets in a graphical format if the Javascript output was
 * enabled.
 */
static void dump_raw_message (const char *descr, const uint8_t *msg, const uint16_t *m, uint32_t offset)
{
  static bool js_ok = true;
  errorinfo *ei = NULL;
  uint32_t   csum;
  int        j, len, msgtype = msg[0] >> 3;

  if (msgtype == 17)
  {
    len  = modeS_message_len_by_type (msgtype);
    csum = crc_checksum (msg, len);
    ei   = crc_checksum_diagnose (csum, len);
  }
  if (Modes.debug & DEBUG_JS)
  {
    if (js_ok && !dump_raw_message_JS (descr, msg, m, offset, ei))
       js_ok = false;       /* don't call this again */
    return;
  }

  EnterCriticalSection (&Modes.print_mutex);

  printf ("\n--- %s\n    ", descr);
  for (j = 0; j < MODES_LONG_MSG_BYTES; j++)
  {
    printf ("%02x",msg[j]);
    if (j == MODES_SHORT_MSG_BYTES-1)
       printf (" ... ");
  }
  printf (" (DF %d, Fixable: %d)\n", msgtype, ei ? ei->errors : 0);
  dump_mag_vector (m, offset);
  printf ("---\n\n");

  LeaveCriticalSection (&Modes.print_mutex);
}

/**
 * Return -1 if the message is out of phase left-side
 * Return  1 if the message is out of phase right-side
 * Return  0 if the message is not particularly out of phase.
 *
 * Note: this function will access m[-1], so the caller should make sure to
 * call it only if we are not at the start of the current buffer.
 */
static int detect_out_of_phase (const uint16_t *preamble)
{
  if (preamble[3] > preamble[2]/3)
     return (1);

  if (preamble[10] > preamble[9]/3)
     return (1);

  if (preamble[6] > preamble[7]/3)
     return (-1);

#if 0
  if (preamble[-1] > preamble[1]/3)
     return (-1);
#else
  /*
   * Apply this important PR from:
   *   https://github.com/MalcolmRobb/dump1090/pull/100/files
   */
  if (preamble[-1] > preamble[0]/3)
     return (-1);
#endif

  return (0);
}

static uint16_t clamped_scale (uint16_t v, uint16_t scale)
{
  uint32_t scaled = (uint32_t)v * scale / 16384;

  if (scaled > 65535)
     return (65535);
  return (uint16_t) scaled;
}

/**
 * This function decides whether we are sampling early or late,
 * and by approximately how much, by looking at the energy in
 * preamble bits before and after the expected pulse locations.
 *
 * It then deals with one sample pair at a time, comparing samples
 * to make a decision about the bit value. Based on this decision it
 * modifies the sample value of the *adjacent* sample which will
 * contain some of the energy from the bit we just inspected:
 *
 * \li `payload [0]` should be the start of the preamble,
 * \li `payload [-1 .. MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 1]` should be accessible.
 * \li `payload [MODES_PREAMBLE_SAMPLES .. MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 1]` will be updated.
 */
static void apply_phase_correction (uint16_t *payload)
{
  int j;

  /* we expect 1 bits at 0, 2, 7, 9
   * and 0 bits at -1, 1, 3, 4, 5, 6, 8, 10, 11, 12, 13, 14
   * use bits -1,6 for early detection (bit 0/7 arrived a little early, our sample period starts after the bit phase so we include some of the next bit)
   * use bits 3,10 for late detection (bit 2/9 arrived a little late, our sample period starts before the bit phase so we include some of the last bit)
   */
  uint32_t onTime = (payload[0]  + payload[2] + payload[7] + payload[9]);
  uint32_t early  = (payload[-1] + payload[6]) << 1;
  uint32_t late   = (payload[3]  + payload[10]) << 1;

  if (onTime == 0 && early == 0 && late == 0)
  {
    /* Blah, can't do anything with this, avoid a divide-by-zero */
    return;
  }

  if (early > late)
  {
    /* Our sample period starts late and so includes some of the next bit.
     */
    uint16_t scaleUp = 16384 + 16384 * early / (early + onTime);   /* 1 + early / (early+onTime) */
    uint16_t scaleDown = 16384 - 16384 * early / (early + onTime); /* 1 - early / (early+onTime) */

    /* trailing bits are 0; final data sample will be a bit low.
     */
    payload [MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 1] =
        clamped_scale (payload[MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 1],  scaleUp);

    for (j = MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 2; j > MODES_PREAMBLE_SAMPLES; j -= 2)
    {
      if (payload[j] > payload[j+1])
      {
        /* x [1 0] y
         * x overlapped with the "1" bit and is slightly high
         */
        payload [j-1] = clamped_scale (payload[j-1], scaleDown);
      }
      else
      {
        /* x [0 1] y
         * x overlapped with the "0" bit and is slightly low
         */
        payload [j-1] = clamped_scale (payload[j-1], scaleUp);
      }
    }
  }
  else
  {
    /* Our sample period starts early and so includes some of the previous bit.
     */
    uint16_t scaleUp = 16384 + 16384 * late / (late + onTime);    /* 1 + late / (late+onTime) */
    uint16_t scaleDown = 16384 - 16384 * late / (late + onTime);  /* 1 - late / (late+onTime) */

    /* leading bits are 0; first data sample will be a bit low.
     */
    payload [MODES_PREAMBLE_SAMPLES] = clamped_scale (payload[MODES_PREAMBLE_SAMPLES], scaleUp);
    for (j = MODES_PREAMBLE_SAMPLES; j < MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES - 2; j += 2)
    {
      if (payload[j] > payload[j+1])
      {
        /* x [1 0] y
         * y overlapped with the "0" bit and is slightly low
         */
        payload [j+2] = clamped_scale (payload[j+2], scaleUp);
      }
      else
      {
        /* x [0 1] y
         * y overlapped with the "1" bit and is slightly high
         */
        payload [j+2] = clamped_scale (payload[j+2], scaleDown);
      }
    }
  }
}

/**
 * Detect a Mode S messages inside the magnitude buffer pointed by `mag` and of
 * size `mlen` bytes. Every detected Mode S message is convert it into a
 * stream of bits and passed to the function to display it.
 */
void demod_2000 (const mag_buf *mag)
{
  modeS_message mm;
  uint8_t       msg [MODES_LONG_MSG_BYTES], *msg_ptr;
  uint16_t      aux [MODES_PREAMBLE_SAMPLES + MODES_LONG_MSG_SAMPLES + 1];
  uint32_t      j;
  bool          use_correction = false;
  u_int         mlen = mag->valid_length - mag->overlap;
  uint16_t     *m = mag->data;

  memset (&mm, '\0', sizeof(mm));

  /**
   * The Mode S preamble is made of impulses of 0.5 microseconds at
   * the following time offsets:
   *
   * \li 0   - 0.5 usec: first impulse.
   * \li 1.0 - 1.5 usec: second impulse.
   * \li 3.5 - 4   usec: third impulse.
   * \li 4.5 - 5   usec: last impulse.
   *
   * Since we are sampling at 2 MHz every sample in our magnitude vector
   * is 0.5 usec, so the preamble will look like this, assuming there is
   * an impulse at offset 0 in the array:
   *```
   *  0   -----------------
   *  1   -
   *  2   ------------------
   *  3   --
   *  4   -
   *  5   --
   *  6   -
   *  7   ------------------
   *  8   --
   *  9   -------------------
   *```
   */
  for (j = 0; j < mlen; j++)
  {
    int       high, i, errors, errors56, errors_ty;
    uint16_t *preamble, *payload, *ptr;
    uint8_t   the_byte, the_err;
    int       msg_len, scan_len;
    uint32_t  sig_level, noise_level;
    uint16_t  snr;
    bool      message_ok;

    preamble = m + j;
    payload  = m + j + MODES_PREAMBLE_SAMPLES;

    /* Rather than clear the whole mm structure, just clear the parts which are required. The clear
     * is required for every bit of the input stream, and we don't want to be memset-ing the whole
     * modeS_message structure two million times per second if we don't have to..
     */
    mm.AC_flags   = 0;
    mm.error_bits = 0;

    /* This is not a re-try with phase correction.
     * So try to find a new preamble
     */
    if (!use_correction)
    {
      if (Modes.mode_AC)  /* \todo move this into the main_data_loop() */
      {
        int ModeA = detect_mode_A (preamble, &mm);

        if (ModeA) /* We have found a valid ModeA/C in the data */
        {
          mm.timestamp_msg = mag->sample_timestamp + ((j+1) * 6);

          /* compute message receive time as block-start-time + difference in the 12MHz clock
           */
          mm.sys_timestamp_msg = mag->sys_timestamp + receiveclock_ms_elapsed (mag->sample_timestamp, mm.timestamp_msg);

          /* Decode the received message
           */
          decode_mode_A_message (&mm, ModeA);

          /* Pass data to the next layer
           */
          modeS_user_message (&mm);

          j += MODEAC_MSG_SAMPLES;
          Modes.stat.demod_modeac++;
          continue;
        }
      }   /* end \todo */

      /* First check of relations between the first 10 samples
       * representing a valid preamble.
       * We don't even investigate further if this simple test is not passed
       */
      if (!(preamble[0] > preamble[1] &&
            preamble[1] < preamble[2] &&
            preamble[2] > preamble[3] &&
            preamble[3] < preamble[0] &&
            preamble[4] < preamble[0] &&
            preamble[5] < preamble[0] &&
            preamble[6] < preamble[0] &&
            preamble[7] > preamble[8] &&
            preamble[8] < preamble[9] &&
            preamble[9] > preamble[6]))
      {
        if ((Modes.debug & DEBUG_NOPREAMBLE) && preamble[0] > DEBUG_NOPREAMBLE_LEVEL)
           dump_raw_message ("Unexpected ratio among first 10 samples", msg, m, j);
        continue;
      }

      /* The samples between the two spikes must be < than the average
       * of the high spikes level. We don't test bits too near to
       * the high levels as signals can be out of phase so part of the
       * energy can be in the near samples
       */
      high = (preamble[0] + preamble[2] + preamble[7] + preamble[9]) / 6;
      if (preamble[4] >= high || preamble[5] >= high)
      {
        if ((Modes.debug & DEBUG_NOPREAMBLE) && preamble[0] > DEBUG_NOPREAMBLE_LEVEL)
           dump_raw_message ("Too high level in samples between 3 and 6", msg, m, j);
        continue;
      }

      /* Similarly samples in the range 11-14 must be low, as it is the
       * space between the preamble and real data. Again we don't test
       * bits too near to high levels, see above
       */
      if (preamble[11] >= high ||
          preamble[12] >= high ||
          preamble[13] >= high ||
          preamble[14] >= high)
      {
        if ((Modes.debug & DEBUG_NOPREAMBLE) && preamble[0]  > DEBUG_NOPREAMBLE_LEVEL)
            dump_raw_message ("Too high level in samples between 10 and 15", msg, m, j);
        continue;
      }
      Modes.stat.valid_preamble++;
    }
    else
    {
      /* If the previous attempt with this message failed, retry using
       * magnitude correction
       * Make a copy of the Payload, and phase correct the copy
       */
      memcpy (aux, &preamble[-1], sizeof(aux));
      apply_phase_correction (&aux[1]);
      payload = &aux[1 + MODES_PREAMBLE_SAMPLES];

      /* TODO ... apply other kind of corrections
       */
    }

    /* Decode all the next 112 bits, regardless of the actual message
     * size. We'll check the actual message type later
     */
    msg_ptr   = &msg [0];
    ptr       = payload;
    the_byte  = 0;
    the_err   = 0;
    errors_ty = errors = errors56 = 0;

    /* We should have 4 'bits' of 0/1 and 1/0 samples in the preamble,
     * so include these in the signal strength
     */
    sig_level = preamble[0] + preamble[2] + preamble[7] + preamble[9];
    noise_level = preamble[1] + preamble[3] + preamble[4] + preamble[6] + preamble[8];

    msg_len = scan_len = MODES_LONG_MSG_BITS;

    for (i = 0; i < scan_len; i++)
    {
      uint32_t a = *ptr++;
      uint32_t b = *ptr++;

      if (a > b)
      {
        the_byte |= 1;
        if (i < 56)
        {
          sig_level += a;
          noise_level += b;
        }
      }
      else if (a < b)
      {
        /* the_byte |= 0;*/
        if (i < 56)
        {
          sig_level += b;
          noise_level += a;
        }
      }
      else
      {
        if (i < 56)
        {
          sig_level   += a;
          noise_level += a;
        }

        if (i >= MODES_SHORT_MSG_BITS)      /* (a == b), and we're in the long part of a frame */
        {
          errors++;
       /* the_byte |= 0; */
        }
        else if (i >= 5)                    /* (a == b), and we're in the short part of a frame */
        {
          scan_len = MODES_LONG_MSG_BITS;
          errors56 = ++errors;
       /* the_byte |= 0; */
        }
        else if (i)                         /* (a == b), and we're in the message type part of a frame */
        {
          errors_ty = errors56 = ++errors;
          the_err  |= 1;
       /* the_byte |= 0; */
        }
        else                                /* (a == b), and we're in the first bit of the message type part of a frame */
        {
          errors_ty = errors56 = ++errors;
          the_err  |= 1;
          the_byte |= 1;
        }
      }

      if ((i & 7) == 7)
      {
        *msg_ptr++ = the_byte;
      }
      else if (i == 4)
      {
        msg_len = modeS_message_len_by_type (the_byte);
        if (errors == 0)
        {
          scan_len = msg_len;
          Modes.stat.demodulated++;
        }
      }

      the_byte = the_byte << 1;
      if (i < 7)
         the_err = the_err << 1;

      /* If we've exceeded the permissible number of encoding errors, abandon ship now
       */
      if (errors > MODES_MSG_ENCODER_ERRS)
      {
        if (i < MODES_SHORT_MSG_BITS)
        {
          msg_len = 0;
        }
        else if ((errors_ty == 1) && (the_err == 0x80))
        {
          /* If we only saw one error in the first bit of the byte of the frame, then it's possible
           * we guessed wrongly about the value of the bit. We may be able to correct it by guessing
           * the other way.
           *
           * We guessed a '1' at bit 7, which is the DF length bit == 112 Bits.
           * Inverting bit 7 will change the message type from a long to a short.
           * Invert the bit, cross your fingers and carry on.
           */
          msg_len = MODES_SHORT_MSG_BITS;
          msg[0]   ^= the_err;
          errors_ty = 0;
          errors    = errors56;    /* revert to the number of errors prior to bit 56 */
        }
        else if (i < MODES_LONG_MSG_BITS)
        {
          msg_len = MODES_SHORT_MSG_BITS;
          errors  = errors56;
        }
        else
        {
          msg_len = MODES_LONG_MSG_BITS;
        }
        break;
      }
    }

    /* Ensure msg_len is consistent with the DF type
     */
    if (msg_len > 0)
    {
      i = modeS_message_len_by_type (msg[0] >> 3);
      if (msg_len > i)
         msg_len = i;
      else if (msg_len < i)
         msg_len = 0;
    }

    /* If we guessed at any of the bits in the DF type field, then look to see if our guess was sensible.
     * Do this by looking to see if the original guess results in the DF type being one of the ICAO defined
     * message types. If it isn't then toggle the guessed bit and see if this new value is ICAO defined.
     * if the new value is ICAO defined, then update it in our message.
     */
    if ((msg_len) && (errors_ty == 1) && (the_err & 0x78))
    {
      /* We guessed at one (and only one) of the message type bits. See if our guess is "likely"
       * to be correct by comparing the DF against a list of known good DF's
       */
      int      this_DF       = ((the_byte = msg[0]) >> 3) & 0x1f;
      uint32_t valid_DF_bits = 0x017F0831;    /* One bit per 32 possible DF's. Set bits 0,4,5,11,16.17.18.19,20,21,22,24 */
      uint32_t this_DF_bit   = (1 << this_DF);

      if (0 == (valid_DF_bits & this_DF_bit))
      {
        /* The current DF is not ICAO defined, so is probably an errors.
         * Toggle the bit we guessed at and see if the resultant DF is more likely
         */
        the_byte   ^= the_err;
        this_DF     = (the_byte >> 3) & 0x1f;
        this_DF_bit = (1 << this_DF);

        /* if this DF any more likely?
         */
        if (valid_DF_bits & this_DF_bit)
        {
          /* Yep, more likely, so update the main message
           */
          msg [0] = the_byte;
          errors--;      /* decrease the error count so we attempt to use the modified DF */
          Modes.stat.demodulated++;
        }
      }
    }

   /*
    * snr = 5 * 20 * log10 (sig_level / noise_level)       (in units of 0.2dB)
    *     = 100 * log10 (sig_level) - 100 * log10 (noise_level)
    */
    while (sig_level > 65535 || noise_level > 65535)
    {
      sig_level   >>= 1;
      noise_level >>= 1;
    }
    snr = Modes.log10_lut [sig_level] - Modes.log10_lut [noise_level];

    /* When we reach this point, if error is small, and the signal strength is large enough
     * we may have a Mode S message on our hands. It may still be broken and the CRC may not
     * be correct, but this can be handled by the next layer.
     */
    if (msg_len && ((2 * snr) > (int) (MODES_MSG_SQUELCH_DB * 10)) &&  errors <= MODES_MSG_ENCODER_ERRS)
    {
      int result;

      /* Set initial mm structure details
       */
      mm.timestamp_msg = mag->sample_timestamp + (j * 6);

      /* Compute message receive time as block-start-time + difference in the 12MHz clock
       */
      mm.sys_timestamp_msg = mag->sys_timestamp + receiveclock_ms_elapsed (mag->sample_timestamp, mm.timestamp_msg);

      mm.sig_level = (365.0*60 + sig_level + noise_level) * (365.0*60 + sig_level + noise_level) / MAX_POWER / 60 / 60;

      /* Decode the received message
       */
      result = decode_mode_S_message (&mm, msg);
      if (result < 0)
           message_ok = false;
      else message_ok = true;

      /* Update statistics
       */
      /* \todo? */

      /* Output debug mode info if needed
       */
      if (use_correction)
      {
        if (Modes.debug & DEBUG_DEMOD)
            dump_raw_message ("Demodulated with 0 errors", msg, m, j);

        else if ((Modes.debug & DEBUG_BADCRC) &&  mm.msg_type == 17 && (!message_ok || mm.error_bits > 0))
            dump_raw_message ("Decoded with bad CRC", msg, m, j);

        else if ((Modes.debug & DEBUG_GOODCRC) && message_ok && mm.error_bits == 0)
            dump_raw_message ("Decoded with good CRC", msg, m, j);
      }

      /* Skip this message if we are sure it's fine
       */
      if (message_ok)
      {
        j += 2 * (MODES_PREAMBLE_US + msg_len) - 1;

        /* Pass data to the next layer
         */
        modeS_user_message (&mm);
      }
    }
    else
    {
      message_ok = false;
      if ((Modes.debug & DEBUG_DEMODERR) && use_correction)
      {
        printf ("The following message has %d demod errors\n", errors);
        dump_raw_message ("Demodulated with errors", msg, m, j);
      }
    }

    /* Retry with phase correction if enabled, necessary and possible.
     */
    if (Modes.phase_enhance && (!message_ok || mm.error_bits > 0) && !use_correction && j && detect_out_of_phase(preamble))
    {
      use_correction = true;
      Modes.stat.out_of_phase++;
      j--;
    }
    else
    {
      use_correction = false;
    }
  }
}

