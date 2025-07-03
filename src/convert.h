/**\file    convert.h
 * \ingroup Samplers
 * \brief   IQ-data converters; convert to magnitude data (to a `mag_buf` structure)
 *
 * Copyright (c) 2015 Oliver Jowett <oliver@mutability.co.uk>
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct convert_state {
        float       DC_A;
        float       DC_B;
        float       z1_I;
        float       z1_Q;
        const char *func_name;
        const char *description;
      } convert_state;

typedef enum convert_format {
        INPUT_ILLEGAL = 0,
        INPUT_UC8,
        INPUT_SC16,
        INPUT_SC16Q11
      } convert_format;

typedef void (*convert_func) (const void    *iq_input,
                              uint16_t      *mag_output,
                              unsigned       nsamples,
                              convert_state *state,
                              double        *out_power);

convert_func convert_init (convert_format  format,
                           double          sample_rate,
                           bool            filter_dc,
                           bool            compute_power,
                           convert_state **state_p);

const char *convert_format_name (convert_format f);

void convert_cleanup (convert_state **state_p);
