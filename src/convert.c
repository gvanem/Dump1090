/**\file    convert.c
 * \ingroup Samplers
 * \brief   IQ-data converters; convert to magnitude data (to a `mag_buf` structure)
 *
 * Copyright (c) 2015 Oliver Jowett <oliver@mutability.co.uk>
 */
#include "convert.h"
#include "misc.h"

static void convert_uc8_nodc_nopower (const void    *iq_data,
                                      uint16_t      *mag_data,
                                      unsigned       nsamples,
                                      convert_state *state,
                                      double        *out_power)
{
  const uint16_t *in = iq_data;
  unsigned        i;

  MODES_NOTUSED (state);
  assert (Modes.mag_lut);

  /* unroll this a bit
   */
  for (i = 0; i < (nsamples >> 3); i++)
  {
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
    *mag_data++ = Modes.mag_lut [*in++];
  }

  for (i = 0; i < (nsamples & 7); i++)
      *mag_data++ = Modes.mag_lut [*in++];

  if (out_power)
     *out_power = 0.0;  /* not measured */
}

static void convert_uc8_nodc_power (const void    *iq_data,
                                    uint16_t      *mag_data,
                                    unsigned       nsamples,
                                    convert_state *state,
                                    double        *out_power)
{
  const uint16_t *in = iq_data;
  unsigned        i;
  uint64_t        power = 0;
  uint16_t        mag;

  MODES_NOTUSED (state);
  assert (Modes.mag_lut);

  /* unroll this a bit
   */
  for (i = 0; i < (nsamples >> 3); i++)
  {
    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;

    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;
  }

  for (i = 0; i < (nsamples & 7); i++)
  {
    mag = Modes.mag_lut [*in++];
    *mag_data++ = mag;
    power += (uint32_t)mag * (uint32_t)mag;
  }

  if (out_power)
     *out_power = power / 65535.0 / 65535.0;
}

static void convert_uc8_generic (const void    *iq_data,
                                 uint16_t      *mag_data,
                                 unsigned       nsamples,
                                 convert_state *state,
                                 double        *out_power)
{
  const uint8_t *in    = iq_data;
  float          power = 0.0;
  float          z1_I = state->z1_I;
  float          z1_Q = state->z1_Q;
  float          DC_A = state->DC_A;
  float          DC_B = state->DC_B;
  unsigned       i;
  uint8_t        I, Q;
  float          fI, fQ, mag_sq;

  for (i = 0; i < nsamples; i++)
  {
    I  = *in++;
    Q  = *in++;
    fI = (I - 127.5) / 127.5;
    fQ = (Q - 127.5) / 127.5;

    /* DC block
     */
    z1_I = fI * DC_A + z1_I * DC_B;
    z1_Q = fQ * DC_A + z1_Q * DC_B;
    fI -= z1_I;
    fQ -= z1_Q;

    mag_sq = fI * fI + fQ * fQ;
    if (mag_sq > 1)
       mag_sq = 1;

    power += mag_sq;
    *mag_data++ = (uint16_t) (sqrtf(mag_sq) * 65535.0 + 0.5);
  }

  state->z1_I = z1_I;
  state->z1_Q = z1_Q;

  if (out_power)
     *out_power = power;
}

static void convert_sc16_generic (const void    *iq_data,
                                  uint16_t      *mag_data,
                                  unsigned       nsamples,
                                  convert_state *state,
                                  double        *out_power)
{
  const uint16_t *in = iq_data;
  float           power = 0.0;
  float           z1_I = state->z1_I;
  float           z1_Q = state->z1_Q;
  float           DC_A = state->DC_A;
  float           DC_B = state->DC_B;
  unsigned        i;
  int16_t         I, Q;
  float           fI, fQ, mag_sq;

  for (i = 0; i < nsamples; i++)
  {
    I  = (int16_t) *in++;
    Q  = (int16_t) *in++;
    fI = I / 32768.0;
    fQ = Q / 32768.0;

    /* DC block
     */
    z1_I = fI * DC_A + z1_I * DC_B;
    z1_Q = fQ * DC_A + z1_Q * DC_B;
    fI -= z1_I;
    fQ -= z1_Q;

    mag_sq = fI * fI + fQ * fQ;
    if (mag_sq > 1)
       mag_sq = 1;

    power += mag_sq;
    *mag_data++ = (uint16_t) (sqrtf(mag_sq) * 65535.0 + 0.5);
  }

  state->z1_I = z1_I;
  state->z1_Q = z1_Q;

  if (out_power)
     *out_power = power;
}

static void convert_sc16q11_generic (const void    *iq_data,
                                     uint16_t      *mag_data,
                                     unsigned       nsamples,
                                     convert_state *state,
                                     double        *out_power)
{
  const uint16_t *in = iq_data;
  float           power = 0.0;
  float           z1_I = state->z1_I;
  float           z1_Q = state->z1_Q;
  float           DC_A = state->DC_A;
  float           DC_B = state->DC_B;
  unsigned        i;
  int16_t         I, Q;
  float           fI, fQ, mag_sq;

  for (i = 0; i < nsamples; i++)
  {
    I  = (int16_t) *in++;
    Q  = (int16_t) *in++;
    fI = I / 2048.0;
    fQ = Q / 2048.0;

    /* DC block
     */
    z1_I = fI * DC_A + z1_I * DC_B;
    z1_Q = fQ * DC_A + z1_Q * DC_B;
    fI -= z1_I;
    fQ -= z1_Q;

    mag_sq = fI * fI + fQ * fQ;
    if (mag_sq > 1)
       mag_sq = 1;

    power += mag_sq;
    *mag_data++ = (uint16_t) (sqrtf(mag_sq) * 65535.0 + 0.5);
  }

  state->z1_I = z1_I;
  state->z1_Q = z1_Q;

  if (out_power)
     *out_power = power;
}

#define C1 12868
#define C2 36646
#define C3 54842
#define C4 64692
#define T1 106
#define T2 618

static void convert_sc16_nodc_nopower (const void    *iq_data,
                                       uint16_t      *mag_data,
                                       unsigned       nsamples,
                                       convert_state *state,
                                       double        *out_power)
{
  const uint16_t *in = iq_data;
  unsigned        i;
  uint32_t        I, Q, mag;

  MODES_NOTUSED (state);

  for (i = 0; i < nsamples; i++)
  {
    uint32_t thresh;

    I = abs ((int16_t) *in++);
    Q = abs ((int16_t) *in++);

    if (I < Q)   /* about 1% error */
    {
      thresh = (T1 * Q) >> 8;
      if (I < thresh)
           mag = (C1 * I + C4 * Q) >> 16;
      else mag = (C2 * I + C3 * Q) >> 16;
    }
    else
    {
      thresh = (T2 * Q) >> 8;
      if (I < thresh)
           mag = (C3 * I + C2 * Q) >> 16;
      else mag = (C4 * I + C1 * Q) >> 16;
    }
    *mag_data++ = (uint16_t) mag;
  }

  if (out_power)
     *out_power = 0.0;
}

static const struct {
       convert_format format;
       bool           can_filter_DC;
       bool           can_compute_power;
       convert_func   func;
       const char    *func_name;
       const char    *description;
   } converters_table[] = {      /* In order of preference */
    { INPUT_UC8,     false, false, convert_uc8_nodc_nopower,  "convert_uc8_nodc_nopower",  "UC8, integer/table path" },
    { INPUT_UC8,     false, true,  convert_uc8_nodc_power,    "convert_uc8_nodc_power",    "UC8, integer/table path, with power measurement" },
    { INPUT_UC8,     true,  true,  convert_uc8_generic,       "convert_uc8_generic",       "UC8, float path" },
    { INPUT_SC16,    false, false, convert_sc16_nodc_nopower, "convert_sc16_nodc_nopower", "SC16, integer path" },
    { INPUT_SC16,    true,  true,  convert_sc16_generic,      "convert_sc16_generic",      "SC16, float path" },
    { INPUT_SC16Q11, true,  true,  convert_sc16q11_generic,   "convert_sc16q11_generic",   "SC16Q11, float path" }
  };

convert_func convert_init (convert_format  format,
                           double          sample_rate,
                           bool            filter_dc,
                           bool            compute_power,
                           convert_state **state_p)
{
  convert_state *state;
  unsigned i;

  for (i = 0; i < DIM(converters_table); i++)
  {
    if (converters_table[i].format != format)
       continue;
    if (filter_dc && !converters_table[i].can_filter_DC)
       continue;
    if (compute_power && !converters_table[i].can_compute_power)
       continue;
    break;
  }

  if (i == DIM(converters_table))
  {
    LOG_STDERR ("No suitable converter for format=%d (%s), power=%d, DC=%d\n",
                format, convert_format_name(format), compute_power, filter_dc);
    return (NULL);
  }

  state = malloc (sizeof(*state));
  if (!state)
  {
    LOG_STDERR ("Cannot allocate converter state\n");
    return (NULL);
  }

  state->func_name   = converters_table[i].func_name;
  state->description = converters_table[i].description;
  state->z1_I = 0;
  state->z1_Q = 0;

  if (filter_dc)
  {
    /* init DC block @ 1Hz
     */
    state->DC_B = exp (-2.0 * M_PI * 1.0 / sample_rate);
    state->DC_A = 1.0 - state->DC_B;
  }
  else
  {
    /* if the converter does filtering, make sure it has no effect
     */
    state->DC_B = 1.0;
    state->DC_A = 0.0;
  }
  *state_p = state;
  return (converters_table[i].func);
}

void convert_cleanup (convert_state **state_p)
{
  convert_state *state = *state_p;
  if (state)
     free (state);
  *state_p = NULL;
}

const char *convert_format_name (convert_format f)
{
  return (f == INPUT_ILLEGAL ? "INPUT_ILLEGAL" :
          f == INPUT_UC8     ? "INPUT_UC8"     :
          f == INPUT_SC16    ? "INPUT_SC16"    :
          f == INPUT_SC16Q11 ? "INPUT_SC16Q11" : "?");
}
