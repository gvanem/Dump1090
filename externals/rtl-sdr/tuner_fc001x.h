/*
 * Fitipower FC0012/FC0013 tuner driver
 *
 * Copyright (C) 2012 Hans-Frieder Vogt <hfvogt@gmx.net>
 *
 * modified for use in librtlsdr
 * Copyright (C) 2012 Steve Markgraf <steve@steve-m.de>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef _FC001X_H_
#define _FC001X_H_

#define FC001X_I2C_ADDR		0xc6
#define FC001X_CHECK_ADDR	0x00
#define FC0012_CHECK_VAL	0xa1
#define FC0013_CHECK_VAL	0xa3

int fc0012_init(void *dev);
int fc0013_init(void *dev);
int fc0012_set_freq(void *dev, uint32_t freq);
int fc0013_set_freq(void *dev, uint32_t freq);
int fc001x_set_gain_mode(void *dev, int manual);
int fc0012_set_gain(void *dev, int gain);
int fc0013_set_gain(void *dev, int gain);
int fc001x_set_bw(void *dev, int bw, uint32_t *applied_bw, int apply);
int fc001x_set_i2c_register(void *dev, unsigned i2c_register, unsigned data, unsigned mask);
int fc0012_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength);
int fc0013_get_i2c_register(void *dev, unsigned char *data, int *len, int *strength);
int fc0012_exit(void *dev);
int fc0013_exit(void *dev);
const int *fc001x_get_gains(int *len);

#endif
